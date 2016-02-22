#ifndef PIKA_WORKER_THREAD_H_
#define PIKA_WORKER_THREAD_H_

#include <queue>

#include "worker_thread.h"
#include "pika_define.h"
#include "slash_mutex.h"
#include "pika_client_conn.h"
#include "pika_command.h"

class PikaWorkerThread : public pink::WorkerThread<PikaClientConn>
{
public:
  PikaWorkerThread(int cron_interval = 0);
  virtual ~PikaWorkerThread();
  virtual void CronHandle();

  bool ThreadClientKill(std::string ip_port = "");
  int ThreadClientNum();

  uint64_t thread_querynum() {
    return thread_querynum_;
  }

  uint64_t last_sec_thread_querynum() {
    return last_sec_thread_querynum_;
  }

  CmdPtr GetCmd(const std::string& opt) {
    return GetCmdFromTable(opt, cmds_);
  }
private:
  slash::Mutex mutex_; // protect cron_task_
  std::queue<WorkerCronTask> cron_tasks_;

  uint64_t thread_querynum_;
  uint64_t last_sec_thread_querynum_;

  std::map<std::string, CmdPtr> cmds_;

  void AddCronTask(WorkerCronTask task);
  bool FindClient(std::string ip_port);
  void ClientKill(std::string ip_port);
  void ClientKillAll();
};

#endif
