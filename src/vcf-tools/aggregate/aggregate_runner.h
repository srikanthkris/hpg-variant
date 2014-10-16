/*
 * Copyright (c) 2012-2014 Cristina Yenyxe Gonzalez Garcia (EMBL-EBI)
 * Copyright (c) 2012-2014 Ignacio Medina (EMBL-EBI)
 *
 * This file is part of hpg-variant.
 *
 * hpg-variant is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 *
 * hpg-variant is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with hpg-variant. If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef AGGREGATE_RUNNER_H
#define AGGREGATE_RUNNER_H

#include <assert.h>
#include <limits.h>
#include <math.h>
#include <stdlib.h>

#include <omp.h>

#include <bioformats/vcf/vcf_file_structure.h>
#include <bioformats/vcf/vcf_file.h>
#include <bioformats/vcf/vcf_reader.h>
#include <bioformats/vcf/vcf_stats.h>
#include <bioformats/vcf/vcf_write.h>
#include <commons/file_utils.h>
#include <commons/log.h>
#include <commons/string_utils.h>
#include <containers/array_list.h>
#include <containers/list.h>

#include "hpg_variant_utils.h"
#include "aggregate.h"


typedef struct {
    char *id;
    char *filter;
    char *info;
    float quality;
} variant_auxdata_t;

int run_aggregate(shared_options_data_t *shared_options_data, aggregate_options_data_t *options_data);


char *merge_info_and_stats(char *info, variant_stats_t *stats, int overwrite);

variant_auxdata_t *variant_auxdata_new(vcf_record_t *record);

void variant_auxdata_free(variant_auxdata_t *data);

#endif
