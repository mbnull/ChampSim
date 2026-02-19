#ifndef INORDER_CPU_H
#define INORDER_CPU_H

#include "ooo_cpu.h"

class InOrderCPU : public O3_CPU
{
public:
  template <typename... Bs, typename... Ts>
  explicit InOrderCPU(champsim::core_builder<champsim::core_builder_module_type_holder<Bs...>, champsim::core_builder_module_type_holder<Ts...>> b)
      : O3_CPU(b)
  {
  }

  long operate() override;

private:
  long inorder_execute();
};

#endif
