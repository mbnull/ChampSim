#include "inorder_cpu.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <fmt/chrono.h>
#include <fmt/core.h>

std::chrono::seconds elapsed_time();

constexpr long long INORDER_STAT_PRINTING_PERIOD = 10000000;

// IF: fetch instructions from trace and L1I (no DIB)
long InOrderCPU::stage_fetch()
{
  long progress{0};
  progress += fetch_instruction();
  // Skip DIB: mark all unchecked instructions as dib_checked so they go to L1I
  for (auto& instr : IFETCH_BUFFER) {
    if (!instr.dib_checked)
      instr.dib_checked = true;
  }
  initialize_instruction();
  return progress;
}

// ID: promote, decode, dispatch
long InOrderCPU::stage_decode()
{
  long progress{0};
  progress += dispatch_instruction();
  progress += decode_instruction();
  progress += promote_to_decode();
  return progress;
}

// EX: in-order execute — stall on unready sources
long InOrderCPU::stage_execute()
{
  long progress{0};

  // Execute before schedule (reverse pipeline order)
  champsim::bandwidth exec_bw{EXEC_WIDTH};
  for (auto rob_it = std::begin(ROB); rob_it != std::end(ROB) && exec_bw.has_remaining(); ++rob_it) {
    if (rob_it->scheduled && !rob_it->executed && rob_it->ready_time <= current_time) {
      bool ready = std::all_of(std::begin(rob_it->source_registers), std::end(rob_it->source_registers),
                               [&alloc = std::as_const(reg_allocator)](auto srcreg) { return alloc.isValid(srcreg); });
      if (!ready)
        break;
      do_execution(*rob_it);
      exec_bw.consume();
    } else if (!rob_it->executed) {
      break;
    }
  }
  progress += exec_bw.amount_consumed();

  progress += schedule_instruction();
  return progress;
}

// MEM: handle loads/stores and memory returns
long InOrderCPU::stage_memory()
{
  long progress{0};
  progress += handle_memory_return();
  progress += operate_lsq();
  return progress;
}

// WB: complete and retire
long InOrderCPU::stage_writeback()
{
  long progress{0};
  progress += retire_rob();
  progress += complete_inflight_instruction();
  return progress;
}

long InOrderCPU::operate()
{
  long progress{0};

  // Reverse pipeline order
  progress += stage_writeback();
  progress += stage_execute();
  progress += stage_memory();
  progress += stage_decode();
  progress += stage_fetch();

  // heartbeat
  if (show_heartbeat && (num_retired >= (last_heartbeat_instr + INORDER_STAT_PRINTING_PERIOD))) {
    using double_duration = std::chrono::duration<double, typename champsim::chrono::picoseconds::period>;
    auto heartbeat_instr{std::ceil(num_retired - last_heartbeat_instr)};
    auto heartbeat_cycle{double_duration{current_time - last_heartbeat_time} / clock_period};

    auto phase_instr{std::ceil(num_retired - begin_phase_instr)};
    auto phase_cycle{double_duration{current_time - begin_phase_time} / clock_period};

    fmt::print("Heartbeat CPU {} instructions: {} cycles: {} heartbeat IPC: {:.4g} cumulative IPC: {:.4g} (Simulation time: {:%H hr %M min %S sec})\n", cpu,
               num_retired, current_time.time_since_epoch() / clock_period, heartbeat_instr / heartbeat_cycle, phase_instr / phase_cycle, elapsed_time());

    last_heartbeat_instr = num_retired;
    last_heartbeat_time = current_time;
  }

  return progress;
}
