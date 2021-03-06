/*
 * Copyright (c) 2019 Carnegie Mellon University,
 * Copyright (c) 2019 Triad National Security, LLC, as operator of
 *     Los Alamos National Laboratory.
 * All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * with the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 * 3. Neither the name of CMU, TRIAD, Los Alamos National Laboratory, LANL, the
 *    U.S. Government, nor the names of its contributors may be used to endorse
 *    or promote products derived from this software without specific prior
 *    written permission.

 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS "AS IS" AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO
 * EVENT SHALL THE COPYRIGHT HOLDERS OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "hstg.h"

#include <mpi.h>

/* clang-format off */
static const double BUCKET_LIMITS[MON_NUM_BUCKETS] = {
  1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 12, 14, 16, 18, 20, 25, 30, 35, 40, 45,
  50, 60, 70, 80, 90, 100, 120, 140, 160, 180, 200, 250, 300, 350, 400, 450,
  500, 600, 700, 800, 900, 1000, 1200, 1400, 1600, 1800, 2000, 2500, 3000,
  3500, 4000, 4500, 5000, 6000, 7000, 8000, 9000, 10000, 12000, 14000,
  16000, 18000, 20000, 25000, 30000, 35000, 40000, 45000, 50000, 60000,
  70000, 80000, 90000, 100000, 120000, 140000, 160000, 180000, 200000,
  250000, 300000, 350000, 400000, 450000, 500000, 600000, 700000, 800000,
  900000, 1000000, 1200000, 1400000, 1600000, 1800000, 2000000, 2500000,
  3000000, 3500000, 4000000, 4500000, 5000000, 6000000, 7000000, 8000000,
  9000000, 10000000, 12000000, 14000000, 16000000, 18000000, 20000000,
  25000000, 30000000, 35000000, 40000000, 45000000, 50000000, 60000000,
  70000000, 80000000, 90000000, 100000000, 120000000, 140000000, 160000000,
  180000000, 200000000, 250000000, 300000000, 350000000, 400000000,
  450000000, 500000000, 600000000, 700000000, 800000000, 900000000,
  1000000000, 2000000000, 4000000000.0, 8000000000.0,
  1e200, /* clang-format on */
};

void hstg_reset_min(hstg_t& h) {
  h[2] = BUCKET_LIMITS[MON_NUM_BUCKETS - 1]; /* min */
}

void hstg_reduce(const hstg_t& src, hstg_t& sum, MPI_Comm comm) {
  MPI_Reduce(const_cast<double*>(&src[0]), &sum[0], 1, MPI_DOUBLE, MPI_SUM, 0,
             comm);
  MPI_Reduce(const_cast<double*>(&src[1]), &sum[1], 1, MPI_DOUBLE, MPI_MAX, 0,
             comm);
  MPI_Reduce(const_cast<double*>(&src[2]), &sum[2], 1, MPI_DOUBLE, MPI_MIN, 0,
             comm);
  MPI_Reduce(const_cast<double*>(&src[3]), &sum[3], 1, MPI_DOUBLE, MPI_SUM, 0,
             comm);

  MPI_Reduce(const_cast<double*>(&src[4]), &sum[4], MON_NUM_BUCKETS, MPI_DOUBLE,
             MPI_SUM, 0, comm);
}

void hstg_add(hstg_t& h, double d) {
  int b = 0;
  while (b < MON_NUM_BUCKETS - 1 && BUCKET_LIMITS[b] <= d) {
    b++;
  }
  h[4 + b] += 1.0;
  h[0] += 1.0;            /* num */
  if (h[1] < d) h[1] = d; /* max */
  if (h[2] > d) h[2] = d; /* min */
  h[3] += d;              /* sum */
}

double hstg_ptile(const hstg_t& h, double p) {
  double threshold = h[0] * (p / 100.0);
  double sum = 0;
  for (int b = 0; b < MON_NUM_BUCKETS; b++) {
    sum += h[4 + b];
    if (sum >= threshold) {
      double left_point = (b == 0) ? 0 : BUCKET_LIMITS[b - 1];
      double right_point = BUCKET_LIMITS[b];
      double left_sum = sum - h[4 + b];
      double right_sum = sum;
      double pos = (threshold - left_sum) / (right_sum - left_sum);
      double r = left_point + (right_point - left_point) * pos;
      if (r < h[2]) r = h[2]; /* min */
      if (r > h[1]) r = h[1]; /* max */

      return (r);
    }
  }

  return h[1]; /* max */
}

double hstg_num(const hstg_t& h) { return h[0]; }

double hstg_max(const hstg_t& h) { return h[1]; }

double hstg_min(const hstg_t& h) { return h[2]; }

double hstg_sum(const hstg_t& h) { return h[3]; }

double hstg_avg(const hstg_t& h) {
  if (h[0] < 1.0) return (0);
  return h[3] / h[0];
}
