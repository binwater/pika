// Copyright (c) 2015-present, Qihoo, Inc.  All rights reserved.
// This source code is licensed under the BSD-style license found in the
// LICENSE file in the root directory of this source tree. An additional grant
// of patent rights can be found in the PATENTS file in the same directory.

#include "include/pika_stable_log.h"

#include <glog/logging.h>

#include "include/pika_conf.h"
#include "include/pika_server.h"
#include "include/pika_rm.h"

#include "slash/include/env.h"

extern PikaConf* g_pika_conf;
extern PikaServer* g_pika_server;
extern PikaReplicaManager* g_pika_rm;

StableLog::StableLog(const std::string table_name,
    uint32_t partition_id, const std::string& log_path) :
  purging_(false),
  table_name_(table_name),
  partition_id_(partition_id),
  log_path_(log_path) {
  stable_logger_ = std::shared_ptr<Binlog>(
      new Binlog(log_path_, g_pika_conf->binlog_file_size()));
}

StableLog::~StableLog() {
}

void StableLog::Leave() {
  Close();
  RemoveStableLogDir();
}

void StableLog::Close() {
  stable_logger_->Close();
}

void StableLog::RemoveStableLogDir() {
  std::string logpath = log_path_;
  if (logpath[logpath.length() - 1] == '/') {
    logpath.erase(logpath.length() - 1);
  }
  logpath.append("_deleting/");
  if (slash::RenameFile(log_path_, logpath.c_str())) {
    LOG(WARNING) << "Failed to move log to trash, error: " << strerror(errno);
    return;
  }
  g_pika_server->PurgeDir(logpath);

  LOG(WARNING) << "Partition StableLog: " << table_name_ << ":" << partition_id_
    << " move to trash success";
}

bool StableLog::PurgeStableLogs(uint32_t to, bool manual) {
  // Only one thread can go through
  bool expect = false;
  if (!purging_.compare_exchange_strong(expect, true)) {
    LOG(WARNING) << "purge process already exist";
    return false;
  }
  PurgeStableLogArg *arg = new PurgeStableLogArg();
  arg->to = to;
  arg->manual = manual;
  arg->logger = shared_from_this();
  g_pika_server->
    PurgelogsTaskSchedule(&DoPurgeStableLogs, static_cast<void*>(arg));
  return true;
}

void StableLog::ClearPurge() {
  purging_ = false;
}

void StableLog::DoPurgeStableLogs(void* arg) {
  PurgeStableLogArg* purge_arg = static_cast<PurgeStableLogArg*>(arg);
  purge_arg->logger->PurgeFiles(purge_arg->to, purge_arg->manual);
  purge_arg->logger->ClearPurge();
  delete static_cast<PurgeStableLogArg*>(arg);
}

bool StableLog::PurgeFiles(uint32_t to, bool manual) {
  std::map<uint32_t, std::string> binlogs;
  if (!GetBinlogFiles(&binlogs)) {
    LOG(WARNING) << log_path_ << " Could not get binlog files!";
    return false;
  }

  int delete_num = 0;
  struct stat file_stat;
  int remain_expire_num = binlogs.size() - g_pika_conf->expire_logs_nums();
  std::map<uint32_t, std::string>::iterator it;
  for (it = binlogs.begin(); it != binlogs.end(); ++it) {
    if ((manual && it->first <= to)                                                             // Manual purgelogsto
      || (remain_expire_num > 0)                                                                // Expire num trigger
      || (binlogs.size() - delete_num > 10                                                      // At lease remain 10 files
          && stat(((log_path_ + it->second)).c_str(), &file_stat) == 0
          && file_stat.st_mtime < time(NULL) - g_pika_conf->expire_logs_days() * 24 * 3600)) {  // Expire time trigger
      // We check this every time to avoid lock when we do file deletion
      if (!g_pika_rm->BinlogCloudPurgeFromSMP(table_name_, partition_id_, it->first)) {
        LOG(WARNING) << log_path_ << " Could not purge "<< (it->first) << ", since it is already be used";
        return false;
      }

      // Do delete
      slash::Status s = slash::DeleteFile(log_path_ + it->second);
      if (s.ok()) {
        ++delete_num;
        --remain_expire_num;
      } else {
        LOG(WARNING) << log_path_ << " Purge log file : " << (it->second)
          <<  " failed! error:" << s.ToString();
      }
    } else {
      // Break when face the first one not satisfied
      // Since the binlogs is order by the file index
      break;
    }
  }
  if (delete_num) {
    LOG(INFO) << log_path_ << " Success purge "<< delete_num << "binlog file";
  }
  return true;
}

bool StableLog::GetBinlogFiles(std::map<uint32_t, std::string>* binlogs) {
  std::vector<std::string> children;
  int ret = slash::GetChildren(log_path_, children);
  if (ret != 0) {
    LOG(WARNING) << log_path_ << " Get all files in log path failed! error:"
      << ret;
    return false;
  }

  int64_t index = 0;
  std::string sindex;
  std::vector<std::string>::iterator it;
  for (it = children.begin(); it != children.end(); ++it) {
    if ((*it).compare(0, kBinlogPrefixLen, kBinlogPrefix) != 0) {
      continue;
    }
    sindex = (*it).substr(kBinlogPrefixLen);
    if (slash::string2l(sindex.c_str(), sindex.size(), &index) == 1) {
      binlogs->insert(std::pair<uint32_t, std::string>(static_cast<uint32_t>(index), *it));
    }
  }
  return true;
}

