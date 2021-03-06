/*
   Copyright (C) 2016-2017 Alexey Kopytov <akopytov@gmail.com>

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
*/

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <stdlib.h>
#ifdef HAVE_STRING_H
# include <string.h>
#endif
#ifdef HAVE_STRINGS_H
# include <strings.h>
#endif
#ifdef HAVE_MATH_H
# include <math.h>
#endif

#include "sb_options.h"
#include "sb_rand.h"
#include "sb_logger.h"

#include "sb_ck_pr.h"

TLS sb_rng_state_t sb_rng_state CK_CC_CACHELINE;

/* Exported variables */
int sb_rand_seed; /* optional seed set on the command line */

/* Random numbers command line options */

static sb_arg_t rand_args[] =
{
  SB_OPT("rand-type",
         "random numbers distribution {uniform,gaussian,special,pareto}",
         "special", STRING),
  SB_OPT("rand-spec-iter",
         "number of iterations used for numbers generation", "12", INT),
  SB_OPT("rand-spec-pct",
         "percentage of values to be treated as 'special' "
         "(for special distribution)", "1", INT),
  SB_OPT("rand-spec-res",
         "percentage of 'special' values to use "
         "(for special distribution)", "75", INT),
  SB_OPT("rand-seed",
         "seed for random number generator. When 0, the current time is "
         "used as a RNG seed.", "0", INT),
  SB_OPT("rand-pareto-h", "parameter h for pareto distibution", "0.2", DOUBLE),

  SB_OPT_END
};

static rand_dist_t rand_type;
/* pointer to the default PRNG as defined by --rand-type */
static uint32_t (*rand_func)(uint32_t, uint32_t);
static unsigned int rand_iter;
static unsigned int rand_pct;
static unsigned int rand_res;

/*
  Pre-computed FP constants to avoid unnecessary conversions and divisions at
  runtime.
*/
static double rand_iter_mult;
static double rand_pct_mult;
static double rand_pct_2_mult;
static double rand_res_mult;

/* parameters for Pareto distribution */
static double pareto_h; /* parameter h */
static double pareto_power; /* parameter pre-calculated by h */

/* Unique sequence generator state */
static uint32_t rand_unique_index CK_CC_CACHELINE;
static uint32_t rand_unique_offset;

extern inline uint64_t sb_rand_uniform_uint64(void);
extern inline double sb_rand_uniform_double(void);
extern inline uint64_t xoroshiro_rotl(const uint64_t, int);
extern inline uint64_t xoroshiro_next(uint64_t s[2]);

static void rand_unique_seed(uint32_t index, uint32_t offset);

int sb_rand_register(void)
{
  sb_register_arg_set(rand_args);

  return 0;
}

/* Initialize random numbers generation */

int sb_rand_init(void)
{
  char     *s;

  sb_rand_seed = sb_get_value_int("rand-seed");

  s = sb_get_value_string("rand-type");
  if (!strcmp(s, "uniform"))
  {
    rand_type = DIST_TYPE_UNIFORM;
    rand_func = &sb_rand_uniform;
  }
  else if (!strcmp(s, "gaussian"))
  {
    rand_type = DIST_TYPE_GAUSSIAN;
    rand_func = &sb_rand_gaussian;
  }
  else if (!strcmp(s, "special"))
  {
    rand_type = DIST_TYPE_SPECIAL;
    rand_func = &sb_rand_special;
  }
  else if (!strcmp(s, "pareto"))
  {
    rand_type = DIST_TYPE_PARETO;
    rand_func = &sb_rand_pareto;
  }
  else
  {
    log_text(LOG_FATAL, "Invalid random numbers distribution: %s.", s);
    return 1;
  }

  rand_iter = sb_get_value_int("rand-spec-iter");
  rand_iter_mult = 1.0 / rand_iter;

  rand_pct = sb_get_value_int("rand-spec-pct");
  rand_pct_mult = rand_pct / 100.0;
  rand_pct_2_mult = rand_pct / 200.0;

  rand_res = sb_get_value_int("rand-spec-res");
  rand_res_mult = 100.0 / (100.0 - rand_res);

  pareto_h  = sb_get_value_double("rand-pareto-h");
  pareto_power = log(pareto_h) / log(1.0-pareto_h);

  /* Seed PRNG for the main thread. Worker thread do their own seeding */
  sb_rand_thread_init();

  /* Seed the unique sequence generator */
  rand_unique_seed(random(), random());

  return 0;
}


void sb_rand_print_help(void)
{
  printf("Pseudo-Random Numbers Generator options:\n");

  sb_print_options(rand_args);
}


void sb_rand_done(void)
{
}

/* Initialize thread-local RNG state */

void sb_rand_thread_init(void)
{
  /* We use libc PRNG to seed xoroshiro128+ */
  sb_rng_state[0] = (((uint64_t) random()) << 32) |
    (((uint64_t) random()) & UINT32_MAX);
  sb_rng_state[1] = (((uint64_t) random()) << 32) |
    (((uint64_t) random()) & UINT32_MAX);
}

/*
  Return random number in the specified range with distribution specified
  with the --rand-type command line option
*/

uint32_t sb_rand_default(uint32_t a, uint32_t b)
{
  return rand_func(a,b);
}

/* uniform distribution */

uint32_t sb_rand_uniform(uint32_t a, uint32_t b)
{
  return a + sb_rand_uniform_double() * (b - a + 1);
}

/* gaussian distribution */

uint32_t sb_rand_gaussian(uint32_t a, uint32_t b)
{
  double       sum;
  double       t;
  unsigned int i;

  t = b - a + 1;
  for(i=0, sum=0; i < rand_iter; i++)
    sum += sb_rand_uniform_double() * t;

  return a + (uint32_t) (sum * rand_iter_mult) ;
}

/* 'special' distribution */

uint32_t sb_rand_special(uint32_t a, uint32_t b)
{
  double       sum;
  double       t;
  double       range_size;
  double       res;
  double       d;
  double       rnd;
  unsigned int i;

  t = b - a;

  /* Increase range size for special values. */
  range_size = t * rand_res_mult;

  /* Generate uniformly distributed one at this stage  */
  rnd = sb_rand_uniform_double(); /* Random double in the [0, 1) interval */
  /* Random integer in the [0, range_size) interval */
  res = rnd * range_size;

  /*
    Use gaussian distribution for (100 - rand_res) percent of all generated
    values.
  */
  if (res < t)
  {
    sum = 0.0;

    for(i = 0; i < rand_iter; i++)
      sum += sb_rand_uniform_double();

    return a + sum * t * rand_iter_mult;
  }

  /*
    For the remaining rand_res percent of values use the uniform
    distribution. We map previously generated random double in the [0, 1)
    interval to the rand_pct percent part of the [a, b] interval. Then we move
    the resulting value in the [0, (b-a) * (rand_pct / 100)] interval to the
    center of the original interval [a, b].
  */
  d = t * rand_pct_mult;
  res = rnd * (d + 1);
  res += t / 2 - t * rand_pct_2_mult;

  return a + (uint32_t) res;
}

/* Pareto distribution */

uint32_t sb_rand_pareto(uint32_t a, uint32_t b)
{
  return a + (uint32_t) ((b - a + 1) *
                         pow(sb_rand_uniform_double(), pareto_power));
}

/* Generate random string */

void sb_rand_str(const char *fmt, char *buf)
{
  unsigned int i;

  for (i=0; fmt[i] != '\0'; i++)
  {
    if (fmt[i] == '#')
      buf[i] = sb_rand_uniform('0', '9');
    else if (fmt[i] == '@')
      buf[i] = sb_rand_uniform('a', 'z');
    else
      buf[i] = fmt[i];
  }
}

/*
  Unique random sequence generator. This is based on public domain code from
  https://github.com/preshing/RandomSequence
*/

static uint32_t rand_unique_permute(uint32_t x)
{
  static const uint32_t prime = UINT32_C(4294967291);

  if (x >= prime)
    return x; /* The 5 integers out of range are mapped to themselves. */

  uint32_t residue = ((uint64_t) x * x) % prime;
  return (x <= prime / 2) ? residue : prime - residue;
}


static void rand_unique_seed(uint32_t index, uint32_t offset)
{
  rand_unique_index = rand_unique_permute(rand_unique_permute(index) +
                                          0x682f0161);
  rand_unique_offset = rand_unique_permute(rand_unique_permute(offset) +
                                          0x46790905);
}

/* This is safe to be called concurrently from multiple threads */

uint32_t sb_rand_unique(void)
{
  uint32_t index = ck_pr_faa_32(&rand_unique_index, 1);

  return rand_unique_permute((rand_unique_permute(index) + rand_unique_offset) ^
                             0x5bf03635);
}
