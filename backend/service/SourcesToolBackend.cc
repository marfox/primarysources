// Copyright 2015 Google Inc. All Rights Reserved.
// Author: Sebastian Schaffert <schaffert@google.com>

#include "SourcesToolBackend.h"

#include <boost/iostreams/filtering_streambuf.hpp>
#include <boost/iostreams/copy.hpp>
#include <boost/iostreams/filter/gzip.hpp>
#include <glog/logging.h>

#include <model/Statement.h>
#include <parser/Parser.h>
#include <persistence/Persistence.h>
#include <service/StatementCaching.h>
#include <util/TimeLogger.h>

using wikidata::primarysources::model::ApprovalState;
using wikidata::primarysources::model::Statement;

namespace wikidata {
namespace primarysources {
namespace {
// Create cache key for an entity and dataset; the cache key is used to cache
// all statements of the given dataset having the entity as subject. If dataset
// is "", the cache key refers to all statements and the dataset name "all" will
// be used.
std::string createCacheKey(const std::string &qid, ApprovalState state, const std::string &dataset) {
    std::string result;
    if (dataset == "") {
        result.reserve(qid.size() + 2);
        result += qid;
        result += "-";
        result += std::to_string(state);
    } else {
        result.reserve(qid.size() + dataset.size() + 3);
        result += qid;
        result += "-";
        result += dataset;
        result += "-";
        result += std::to_string(state);
    }
    return result;
}
}  // namespace

SourcesToolBackend::SourcesToolBackend(const cppcms::json::value& config)
    : connstr(build_connection(config["database"])) {
    // trigger initialisation of the singleton
    StatusService();

    // If redis is configured, setup the redis service.
    if (!config["redis"].is_null()) {
        redisSvc.reset(new RedisCacheService(config["redis"]["host"].str(),
                                             config["redis"]["port"].number()));
    }
}

std::vector<Statement> SourcesToolBackend::getStatementsByQID(
        cache_t &cache, const std::string &qid,
        ApprovalState state, const std::string &dataset){
    std::vector<Statement> statements;
    std::string cacheKey = createCacheKey(qid,state,dataset);

    // look up in cache and only hit backend in case of a cache miss
    if(!getCachedEntity(cache, cacheKey, &statements)) {
        cppdb::session sql(connstr); // released when sql is destroyed

        Persistence p(sql);
        statements = p.getStatementsByQID(qid, state, dataset);

        storeCachedEntity(cache, cacheKey, statements);
    }

    return statements;
}

std::vector<Statement> SourcesToolBackend::getRandomStatements(
        cache_t &cache, int count, ApprovalState state) {
    cppdb::session sql(connstr); // released when sql is destroyed

    Persistence p(sql);
    return p.getRandomStatements(count, state);
}

std::vector<Statement> SourcesToolBackend::getAllStatements(
        cache_t& cache, int offset, int limit,
        ApprovalState state,
        const std::string& dataset,
        const std::string& property,
        const model::Value* value) {
    cppdb::session sql(connstr); // released when sql is destroyed

    Persistence p(sql);
    return p.getAllStatements(offset, limit, state, dataset, property, value);
}

Statement SourcesToolBackend::getStatementByID(cache_t& cache, int64_t id) {
    cppdb::session sql(connstr); // released when sql is destroyed

    Persistence p(sql);
    return p.getStatement(id);
}

void SourcesToolBackend::updateStatement(
        cache_t& cache, int64_t id, ApprovalState state, const std::string& user) {
    cppdb::session sql(connstr); // released when sql is destroyed

    Persistence p(sql, true);
    sql.begin();

    try {
        // make sure statement exists; will throw an exception if not
        Statement st = p.getStatement(id);

        // update cache; fetch entities with the QID of the statement and remove from the old state,
        // add to the new state
        for (const std::string dataset : {std::string(""), st.dataset()}) {
            std::vector<model::Statement> oldEntity, newEntity;
            auto oldKey = createCacheKey(st.qid(), st.approval_state(), dataset);
            auto newKey = createCacheKey(st.qid(), state, dataset);
            if (getCachedEntity(cache, oldKey, &oldEntity)) {
                oldEntity.erase(std::remove_if(oldEntity.begin(), oldEntity.end(),
                                               [&st](const model::Statement &s) { return s.id() == st.id(); }),
                                oldEntity.end());
                storeCachedEntity(cache, oldKey, oldEntity);
            }

            // no need to cache duplicates or blacklisted
            if (state != ApprovalState::DUPLICATE && state != ApprovalState::BLACKLISTED) {
                if(getCachedEntity(cache, newKey, &newEntity)) {
                    st.set_approval_state(state);
                    newEntity.push_back(st);
                    storeCachedEntity(cache, newKey, newEntity);
                }
            }
        }

        // update database (after cache, it's slower)
        p.updateStatement(id, state);
        if (state != ApprovalState::DUPLICATE && state != ApprovalState::BLACKLISTED) {
            p.addUserlog(user, id, state);
        }

        sql.commit();

        cache.rise("ACTIVITIES");
        StatusService().SetDirty();
    } catch (PersistenceException const &e) {
        sql.rollback();

        throw e;
    }

}

std::vector<Statement> SourcesToolBackend::getStatementsByRandomQID(
        cache_t& cache, ApprovalState state, const std::string& dataset) {
    cppdb::session sql(connstr); // released when sql is destroyed

    Persistence p(sql);
    std::string qid = p.getRandomQID(state, dataset);

    std::vector<Statement> statements;
    std::string cacheKey = createCacheKey(qid, state, dataset);

    // look up in cache and only hit backend in case of a cache miss
    if(!cache.fetch_data(cacheKey, statements)) {
        cppdb::session sql(connstr); // released when sql is destroyed

        Persistence p(sql);
        statements = p.getStatementsByQID(qid, state, dataset);
        storeCachedEntity(cache, cacheKey, statements);

        StatusService().AddCacheMiss();
    } else {
        StatusService().AddCacheHit();
    }

    return statements;
}

int64_t SourcesToolBackend::importStatements(cache_t& cache, std::istream &_in,
                                             const std::string& dataset, bool gzip, bool dedup) {
    // prepare GZIP input stream
    boost::iostreams::filtering_istreambuf zin;
    if (gzip) {
        zin.push(boost::iostreams::gzip_decompressor());
    }
    zin.push(_in);
    std::istream in(&zin);

    cppdb::session sql(connstr); // released when sql is destroyed

    sql.begin();
    Persistence p(sql, true);

    // get timestamp and use it as upload id
    int64_t upload = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();

    int64_t count = 0, first_id = -1, current_id;
    parser::parseTSV(dataset, upload, in, [&sql, &p, &count, &first_id, &current_id](Statement st)  {
        current_id = p.addStatement(st);

        // remember the ID of the first statement we add for deduplication
        if (first_id == -1) {
            first_id = current_id;
        }

        count++;

        // batch commit
        if(count % 100000 == 0) {
            sql.commit();
            sql.begin();
        }
    });
    sql.commit();

    if (dedup) {
        sql.begin();
        p.markDuplicates(first_id);
        sql.commit();
    }

    ClearCachedEntities(cache);

    return count;
}

void SourcesToolBackend::deleteStatements(cache_t& cache, ApprovalState state) {
    cppdb::session sql(connstr); // released when sql is destroyed

    Persistence p(sql);
    p.deleteStatements(state);

    ClearCachedEntities(cache);
}

model::Status SourcesToolBackend::getStatus(cache_t& cache, const std::string& dataset) {
    return StatusService().Status(dataset);
}


Dashboard::ActivityLog SourcesToolBackend::getActivityLog(cache_t& cache) {

    Dashboard::ActivityLog result;

    if(!cache.fetch_data("ACTIVITIES", result)) {
        cppdb::session sql(connstr); // released when sql is destroyed
        Dashboard::Dashboard dashboard(sql);
        result = dashboard.getActivityLog(7, 12, 10);

        cache.store_data("ACTIVITIES", result, 3600);

        StatusService().AddCacheMiss();
    } else {
        StatusService().AddCacheHit();
    }

    return result;
}


std::vector<std::string> SourcesToolBackend::getDatasets(cache_t& cache) {
    std::vector<std::string> datasets;

    // look up in cache and only hit backend in case of a cache miss
    if(!cache.fetch_data("datasets", datasets)) {
        cppdb::session sql(connstr); // released when sql is destroyed

        Persistence p(sql);
        datasets = p.getDatasets();
        cache.store_data("datasets", datasets, 3600);

        StatusService().AddCacheMiss();
    } else {
        StatusService().AddCacheHit();
    }

    return datasets;
}

bool SourcesToolBackend::getCachedEntity(
        cache_t& cache, const std::string &cacheKey,
        std::vector<model::Statement> *result) {
    if (cache.fetch_data(cacheKey, *result)) {
        StatusService().AddCacheHit();
        return true;
    }

    StatusService().AddCacheMiss();

    if (redisSvc) {
        try {
            model::Statements stmts;
            if (!redisSvc->Get(cacheKey, &stmts)) {
                StatusService().AddRedisMiss();
                return false;
            }

            StatusService().AddRedisHit();

            std::copy(stmts.statements().begin(),
                      stmts.statements().end(),
                      std::back_inserter(*result));

            cache.store_data(cacheKey, *result);
            return true;
        } catch(const CacheException& ex) {
            LOG(ERROR) << "Redis exception when retrieving cache entry '"
                       << cacheKey << "': " << ex.what();
            return false;
        }
    }

    return false;
}

void SourcesToolBackend::evictCachedEntity(
        cache_t& cache, const std::string &cacheKey) {
    cache.rise(cacheKey);

    if (redisSvc) {
        try {
            redisSvc->Evict(cacheKey);
        } catch(const CacheException& ex) {
            LOG(ERROR) << "Redis exception when evicting cache entry '"
                       << cacheKey << "': " << ex.what();
        }
    }

}

void SourcesToolBackend::storeCachedEntity(
        SourcesToolBackend::cache_t &cache, const std::string &cacheKey,
        const std::vector<Statement> &value) {
    cache.store_data(cacheKey, value);

    if (redisSvc) {
        try {
            model::Statements stmts;
            for (const Statement& stmt : value) {
                *stmts.add_statements() = stmt;
            }
            redisSvc->Add(cacheKey, stmts);
        } catch(const CacheException& ex) {
            LOG(ERROR) << "Redis exception when storing cache entry '"
                       << cacheKey << "': " << ex.what();
        }
    }
}

void SourcesToolBackend::UpdateCachedEntities(cache_t &cache) {
    LOG(INFO) << "Start refreshing all cached Redis entries ...";

    cppdb::session sql(connstr); // released when sql is destroyed

    Persistence p(sql);

    auto datasets = p.getDatasets();
    datasets.insert(datasets.begin(), "");

    for (const auto& dataset : datasets) {
        TimeLogger timer("Refreshing cached Redis entries for " +
                         (dataset == "" ? "all datasets" : "dataset " + dataset));

        model::Statements stmts;
        std::string qid = "";

        p.getAllStatements([&](const Statement& s) {
            if (qid != s.qid() && qid != "") {
                // store current batch of statements and clear it
                redisSvc->Add(createCacheKey(qid, ApprovalState::UNAPPROVED, dataset), stmts);
                stmts.clear_statements();
            }
            *stmts.add_statements() = s;
            qid = s.qid();
        }, ApprovalState::UNAPPROVED, dataset);

        if (stmts.statements_size() > 0) {
            redisSvc->Add(createCacheKey(qid, ApprovalState::UNAPPROVED, dataset), stmts);
        }
    }

    LOG(INFO) << "Finished refreshing all cached Redis entries.";
}

void SourcesToolBackend::ClearCachedEntities(cache_t &cache) {
    cache.clear();

    if (redisSvc) {
        try {
            redisSvc->Clear();
        } catch(const CacheException& ex) {
            LOG(ERROR) << "Redis exception when clearing cache: " << ex.what();
        }
    }

}

}  // namespace primarysources
}  // namespace wikidata

