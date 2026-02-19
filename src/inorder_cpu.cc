#include "inorder_cpu.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <fmt/chrono.h>
#include <fmt/core.h>

#include "champsim.h"

std::chrono::seconds elapsed_time();

constexpr long long INORDER_STAT_PRINTING_PERIOD = 10000000;

long InOrderCPU::inorder_execute()
{
  // In-order execution: only fire the oldest unexecuted instruction.
  // If it can't execute (sources not ready), stall — nothing behind it can go.
  champsim::bandwidth exec_bw{EXEC_WIDTH};
  for (auto rob_it = std::begin(ROB); rob_it != std::end(ROB) && exec_bw.has_remaining(); ++rob_it) {
    if (rob_it->scheduled && !rob_it->executed && rob_it->ready_time <= current_time) {
      bool ready = std::all_of(std::begin(rob_it->source_registers), std::end(rob_it->source_registers),
                               [&alloc = std::as_const(reg_allocator)](auto srcreg) { return alloc.isValid(srcreg); });
      if (!ready)
        break; // IN-ORDER STALL: can't skip past this instruction
      do_execution(*rob_it);
      exec_bw.consume();
    } else if (!rob_it->executed) {
      break; // IN-ORDER STALL: unexecuted instruction not yet ready
    }
  }
  return exec_bw.amount_consumed();
}

long InOrderCPU::operate()
{
  long progress{0};

  // Pipeline stages in reverse order (same as O3_CPU, but execute is in-order)
  progress += retire_rob();
  progress += complete_inflight_instruction();
  progress += inorder_execute();              // <-- in-order constraint here
  progress += schedule_instruction();
  progress += handle_memory_return();
  progress += operate_lsq();

  progress += dispatch_instruction();
  progress += decode_instruction();
  progress += promote_to_decode();

  progress += fetch_instruction();
  progress += check_dib();
  initialize_instruction();

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
