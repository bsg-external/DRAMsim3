// Author: Tommy Jung

/*
  OUTPUT KEYS:
  # nop       = no request in this bank.
  # closed    = there is a request in this bank, but the row is closed.
  # act       = activate
  # rd        = read
  # wr        = write
  # pre       = precharge
  # row_miss  = there is a request but row miss. 
  # arb       = there is a row hit, but other bank is accessing (arbitrated)
  # ref       = refresh
  # conf      = there is a row hit, but can't access due to various timing constraints (tWTR, tCCD_S, etc)
*/

#include "blood_graph.h"

namespace dramsim3 {

// default constructor
BloodGraph::BloodGraph(int channel_id, const Config &config)
  : config_(config),
    stat_period_(250) // how often dump periodic stat (in clk cycles);
{
  channel_id_ = channel_id; 
  clk_ = 0;

  is_in_ref_ = false;
  ref_count_ = 0;

  bank_count_ = config_.ranks * config_.banks;
  // Indexed accesses below require live elements, not only allocated capacity.
  read_issued_.resize(bank_count_);
  write_issued_.resize(bank_count_);
  pre_count_.resize(bank_count_);
  act_count_.resize(bank_count_);
  for (int i = 0; i < bank_count_; i++) {
    read_issued_[i] = false;
    write_issued_[i] = false;
    pre_count_[i] = 0;
    act_count_[i] = 0;
  }

#ifdef BLOOD_GRAPH_ENABLE_TRACE
  std::string trace_file_name = "blood_graph_ch" + std::to_string(channel_id_) + ".log";
  trace_.open(trace_file_name, std::ofstream::out);
  trace_ << "time,bank,state" << std::endl;
#endif
 
  data_line_busy_read_ = 0;
  data_line_busy_write_ = 0;
  cmd_issued_ = false; 


  // stat info
  // Format:
  // {timestamp, tag, channel_id, busy, refresh, read, write} 
  stat_busy_count_ = 0;
  stat_read_count_ = 0;
  stat_write_count_ = 0;
  stat_refresh_count_ = 0;
  std::string stat_file_name = "blood_graph_stat.log";
  stat_.open(stat_file_name, std::ofstream::out);
  stat_ << "timestamp,tag,channel_id,busy,refresh,read,write" << std::endl;

  // periodic stat;
  kernel_start_received_ = false;
  std::string periodic_stat_file_name = "blood_graph_periodic_stat.csv";
  periodic_stat_.open(periodic_stat_file_name, std::ofstream::out);
  periodic_stat_ << "timestamp,channel_id,busy,refresh,read,write" << std::endl;
}


void BloodGraph::IsInRefresh(bool ref)
{
  is_in_ref_ = ref;
}


void BloodGraph::IssueCommand(const Command &cmd) {
  if (!is_in_ref_) {
    int bank_id = cmd_queue_->GetQueueIndex(cmd.Rank(), cmd.Bankgroup(), cmd.Bank());

    if (cmd.IsRefresh()) {
      ref_count_ = config_.tRFC;
      cmd_issued_ = true; 
    } else if (cmd.IsRead()) {
      read_issued_[bank_id] = true;
      data_line_busy_read_ = config_.BL/2;
      cmd_issued_ = true; 
    } else if (cmd.IsWrite()) {
      write_issued_[bank_id] = true;
      data_line_busy_write_ = config_.BL/2;
      cmd_issued_ = true; 
    } else if (cmd.cmd_type == CommandType::ACTIVATE) {
      act_count_[bank_id] = config_.tRCDRD;
      cmd_issued_ = true; 
    } else if (cmd.cmd_type == CommandType::PRECHARGE) {
      pre_count_[bank_id] = config_.tRP;
      cmd_issued_ = true; 
    }
  }
}


void BloodGraph::ClockTick() {
  if (is_in_ref_) {
    for (int i = 0; i < bank_count_; i++) {
      PrintTrace(i, "ref");
      act_count_[i] = 0;
      pre_count_[i] = 0;
    }
    stat_refresh_count_++; // for utilization stat.
  } else if (ref_count_ > 0) {
    for (int i = 0; i < bank_count_; i++) {
      PrintTrace(i, "ref");
    }
    ref_count_--;
    stat_refresh_count_++; // for utilization stat.
  } else {

    bool read_issued_found = false;
    bool write_issued_found = false;

    for (int i = 0; i < bank_count_; i++) {
      if (read_issued_[i]) {
        read_issued_found = true;
        break;
      }
    }
    for (int i = 0; i < bank_count_; i++) {
      if (write_issued_[i]) {
        write_issued_found = true;
        break;
      }
    }

    for (int i = 0; i < bank_count_; i++) {
      if (act_count_[i] > 0) {
        PrintTrace(i, "act");
        act_count_[i]--;
      } else if (pre_count_[i] > 0) {
        PrintTrace(i, "pre");
        pre_count_[i]--;
      } else if (read_issued_[i]) {
        PrintTrace(i, "rd");
      } else if (write_issued_[i]) {
        PrintTrace(i, "wr");
      } else {
        if (cmd_queue_->QueueEmpty(i)) {
          PrintTrace(i, "nop");
        } else {
          int ra, bg, ba;
          std::tie(ba, bg, ra) = cmd_queue_->GetBankBankgroupRankFromQueueIndex(i);
          assert(cmd_queue_->GetQueueIndex(ra, bg, ba) == i);
          if (channel_state_->IsRowOpen(ra,bg,ba)) {
            // check if there is any row hit.
            int open_row = channel_state_->OpenRow(ra,bg,ba);
            bool read_row_hit_found = false;
            bool write_row_hit_found = false;
            auto queue = cmd_queue_->GetQueue(ra,bg,ba);

            for (auto cmd_it = queue.begin(); cmd_it != queue.end(); cmd_it++) {
                if (open_row == cmd_it->Row() && cmd_it->IsRead()) {
                  read_row_hit_found = true;
                } else if (open_row == cmd_it->Row() && cmd_it->IsWrite()) {
                  write_row_hit_found = true;
                }
            }

            if (read_row_hit_found) {
              if (read_issued_found) {
                PrintTrace(i, "arb");
              } else {
                PrintTrace(i, "conf");
              }
            } else if (write_row_hit_found) {
              if (write_issued_found) {
                PrintTrace(i, "arb");
              } else {
                PrintTrace(i, "conf");
              }
            } else {
              PrintTrace(i, "row_miss");
            }
          } else {
            PrintTrace(i, "closed");
          }
        }
      }
    }

    if (cmd_issued_ && (data_line_busy_read_ == 0) && (data_line_busy_write_ == 0)) {
      stat_busy_count_++;
    } else {
      bool ready_bank_found = false;
      for (int i = 0; i < bank_count_; i++) {
        int ra, bg, ba;
        std::tie(ba, bg, ra) = cmd_queue_->GetBankBankgroupRankFromQueueIndex(i);
        auto bank_state = channel_state_->GetBankState(ra,bg,ba);
        auto cmd_timing = bank_state.GetCmdTiming();
        if (bank_state.IsRowOpen()) {
          if (cmd_timing[static_cast<int>(CommandType::PRECHARGE)] <= clk_) {
            ready_bank_found = true;
            break;
          } else if (cmd_timing[static_cast<int>(CommandType::READ)] <= clk_) {
            ready_bank_found = true;
            break;
          } else if (cmd_timing[static_cast<int>(CommandType::WRITE)] <= clk_) {
            ready_bank_found = true;
            break;
          }
        } else {
          if (cmd_timing[static_cast<int>(CommandType::ACTIVATE)] <= clk_) {
            ready_bank_found = true;
            break;
          }
        }
      }

      if (!ready_bank_found) {
        stat_busy_count_++;
      } else {
        if (data_line_busy_read_ > 0) {
          stat_read_count_++;
          data_line_busy_read_--;
        }
        if (data_line_busy_write_ > 0) {
          stat_write_count_++;
          data_line_busy_write_--;
        }
      }
    }

  }

  // reset
  cmd_issued_ = false;
  is_in_ref_ = false;
  for (int i = 0; i < bank_count_; i++) {
    read_issued_[i] = false;
    write_issued_[i] = false;
  }
  
  // periodic stat;
  PrintPeriodicStats();
  // increment clk;
  clk_++;
}

void BloodGraph::PrintTrace(int bank_id, const std::string &str)
{
#ifdef BLOOD_GRAPH_ENABLE_TRACE
  trace_ << clk_ << "," << bank_id << "," << str << std::endl;
#endif
}

void BloodGraph::PrintTagStats(uint32_t tag)
{
  // check if tag is kernel start;
  uint32_t tag_type = (tag & 0xc0000000) >> 30;
  if (tag_type == 2) {
    kernel_start_received_ = true;
  }

  // Format:
  // {timestamp, tag, channel_id, busy, refresh, read, write} 
  stat_ << clk_ << ","
        << tag  << ","
        << channel_id_ << ","
        << stat_busy_count_ << ","
        << stat_refresh_count_ << ","
        << stat_read_count_ << ","
        << stat_write_count_
        << std::endl;
}

void BloodGraph::PrintPeriodicStats()
{
  // check if kernel started;
  if (!kernel_start_received_) {
    return;
  }

#ifdef BLOOD_GRAPH_ENABLE_PERIODIC_TRACE
  if (clk_ % stat_period_ == 0) {
    periodic_stat_ << clk_ << ","
      << channel_id_ << ","
      << stat_busy_count_ << ","
      << stat_refresh_count_ << ","
      << stat_read_count_ << ","
      << stat_write_count_
      << std::endl;
  }
#endif
}


} // namespace dramsim3
