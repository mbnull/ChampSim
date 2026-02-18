#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define N 16000

/* Placed in .bss so they live in RAM and generate real load/store addresses */
static uint32_t src[N];
static uint32_t dst[N];

/* Simple array copy: generates N loads (src_mem) + N stores (dst_mem) */
static void array_copy(uint32_t* out, const uint32_t* in, int n)
{
  for (int i = 0; i < n; i++)
    out[i] = in[i];
}

/* Dot product: generates 2*N loads, conditional branches, accumulation */
static uint32_t dot_product(const uint32_t* a, const uint32_t* b, int n)
{
  uint32_t sum = 0;
  for (int i = 0; i < n; i++)
    sum += a[i] * b[i];
  return sum;
}

/* Bubble sort: lots of conditional branches + loads + stores */
static void bubble_sort(uint32_t* arr, int n)
{
  for (int i = 0; i < n - 1; i++) {
    for (int j = 0; j < n - 1 - i; j++) {
      if (arr[j] > arr[j + 1]) {
        uint32_t tmp = arr[j];
        arr[j] = arr[j + 1];
        arr[j + 1] = tmp;
      }
    }
  }
}
int main()
{
  for (int i = 0; i < N; i++)
    src[i] = (uint32_t)(N - i); /* descending so sort has work to do */

  uint32_t* arr_src = malloc(sizeof(uint32_t) * N);
  uint32_t* arr_dst = malloc(sizeof(uint32_t) * N);
  for (int i = 0; i < N; i++)
    arr_src[i] = (uint32_t)(N - i);
  array_copy(arr_dst, arr_src, N);
  bubble_sort(arr_dst, N);

  uint32_t result = dot_product(arr_src, arr_dst, N);

  printf("result=%d\n", result);

  printf("hello world\n");

  return 0;
}