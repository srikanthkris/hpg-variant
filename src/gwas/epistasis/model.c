#include "model.h"


/* **************************
 *       Main pipeline      *
 * **************************/

risky_combination *get_model_from_combination_in_fold(int order, int comb[order], uint8_t **genotypes, 
                                                      int num_genotype_combinations, uint8_t **genotype_combinations, 
                                                      int num_counts, int counts_aff[num_counts], int counts_unaff[num_counts],
                                                      masks_info info, risky_combination *risky_scratchpad) {
    risky_combination *risky_comb = NULL;
    
    // Get counts for the provided genotypes
    uint8_t *masks = set_genotypes_masks(order, genotypes, info.num_combinations_in_a_row, info); // Grouped by SNP
    combination_counts(order, masks, genotype_combinations, num_genotype_combinations, counts_aff, counts_unaff, info);
    
    // Get high risk pairs for those counts
    void *aux_info;
    int num_risky;
    int *risky_idx = choose_high_risk_combinations(counts_aff, counts_unaff, num_counts, info.num_affected, info.num_unaffected, 
                                                   &num_risky, &aux_info, mdr_high_risk_combinations);
    
    // Filter non-risky SNP combinations
    if (num_risky > 0) {
        // Put together the info about the SNP combination and its genotype combinations
        if (risky_scratchpad) {
            risky_comb = risky_combination_copy(order, comb, genotype_combinations, num_risky, risky_idx, aux_info, risky_scratchpad);
        } else {
            risky_comb = risky_combination_new(order, comb, genotype_combinations, num_risky, risky_idx, aux_info);
        }
    }
    
    free(risky_idx);
    
    return risky_comb;
}


double test_model(int order, risky_combination *risky_comb, uint8_t **val, masks_info info, unsigned int *conf_matrix) {
    // Get the matrix containing {FP,FN,TP,TN}
    confusion_matrix(order, risky_comb, info, val, conf_matrix);
    
    // Evaluate the model, basing on the confusion matrix
    double eval = evaluate_model(conf_matrix, BA);
    risky_comb->accuracy = eval;
    
    return eval;
}


int add_to_model_ranking(risky_combination *risky_comb, int max_ranking_size, linked_list_t *ranking_risky, risky_combination **removed) {
    // Step 6 -> Construct ranking of the best N combinations
    risky_combination *last_element = (linked_list_size(ranking_risky) > 0) ? linked_list_get_last(ranking_risky) : NULL;
    size_t current_ranking_size = ranking_risky->size;
    
    linked_list_iterator_t* iter = linked_list_iterator_new(ranking_risky);
    risky_combination *element = NULL;
    
    if (current_ranking_size > 0) {
        if (last_element) {
            LOG_DEBUG_F("To insert %.3f\tRanking's last is %.3f\n", risky_comb->accuracy, last_element->accuracy);
        } else {
            LOG_DEBUG_F("To insert %.3\n", risky_comb->accuracy );
        }
        
        // If accuracy is not greater than the last element, don't bother inserting
        if (risky_comb->accuracy > last_element->accuracy) {
            int position = 0;
            while (element = linked_list_iterator_curr(iter)) {
                LOG_DEBUG_F("To insert %.3f\tIn ranking (pos #%d) %.3f\n", risky_comb->accuracy, position, element->accuracy);
                if (risky_comb->accuracy > element->accuracy) {
                    linked_list_iterator_insert(risky_comb, iter);
                    
                    if (current_ranking_size >= max_ranking_size) {
                        linked_list_iterator_last(iter);
                        *removed = linked_list_iterator_remove(iter);
                    }
                    
                    linked_list_iterator_free(iter);
                    return position;
                }
                element = linked_list_iterator_next(iter);
                position++;
            }
        }
        
        if (current_ranking_size < max_ranking_size) {
            LOG_DEBUG_F("To insert %.3f at the end", risky_comb->accuracy);
            linked_list_insert_last(risky_comb, ranking_risky);
            linked_list_iterator_free(iter);
            return ranking_risky->size - 1;
        }
    } else {
        linked_list_insert_last(risky_comb, ranking_risky);
        linked_list_iterator_free(iter);
        return ranking_risky->size - 1;
    }
    
    linked_list_iterator_free(iter);
    
    return -1;
}


/* **************************
 *          Counts          *
 * **************************/

void combination_counts(int order, uint8_t *masks, uint8_t **genotype_permutations, int num_genotype_permutations, 
                        int *counts_aff, int *counts_unaff, masks_info info) {
    uint8_t *permutation;
    int count = 0;
    
    __m128i snp_and, snp_cmp;
    
    for (int rc = 0; rc < info.num_combinations_in_a_row; rc++) {
        uint8_t *rc_masks = info.masks + rc * order * NUM_GENOTYPES * info.num_samples_per_mask;
        for (int c = 0; c < num_genotype_permutations; c++) {
            permutation = genotype_permutations[c];
    //         print_gt_combination(comb, c, order);
            count = 0;

            for (int i = 0; i < info.num_affected; i += 16) {
                // Aligned loading
                snp_and = _mm_load_si128(rc_masks + permutation[0] * info.num_samples_per_mask + i);

                // Perform AND operation with all SNPs in the combination
                for (int j = 0; j < order; j++) {
                    snp_cmp = _mm_load_si128(rc_masks + j * NUM_GENOTYPES * info.num_samples_per_mask + 
                                             permutation[j] * info.num_samples_per_mask + i);
                    snp_and = _mm_and_si128(snp_and, snp_cmp);
                }

                count += _mm_popcnt_u64(_mm_extract_epi64(snp_and, 0)) + 
                         _mm_popcnt_u64(_mm_extract_epi64(snp_and, 1));
            }

            LOG_DEBUG_F("aff comb idx (%d) = %d\n", c, count / 8);
            counts_aff[rc * info.num_counts_per_combination + c] = count / 8;

            count = 0;

            for (int i = 0; i < info.num_unaffected; i += 16) {
                // Aligned loading
                snp_and = _mm_load_si128(rc_masks + permutation[0] * info.num_samples_per_mask + info.num_affected_with_padding + i);

                // Perform AND operation with all SNPs in the combination
                for (int j = 0; j < order; j++) {
                    snp_cmp = _mm_load_si128(rc_masks + j * NUM_GENOTYPES * info.num_samples_per_mask + 
                                             permutation[j] * info.num_samples_per_mask + info.num_affected_with_padding + i);
                    snp_and = _mm_and_si128(snp_and, snp_cmp);
                }

                count += _mm_popcnt_u64(_mm_extract_epi64(snp_and, 0)) + 
                         _mm_popcnt_u64(_mm_extract_epi64(snp_and, 1));
            }

            LOG_DEBUG_F("unaff comb idx (%d) = %d\n", c, count / 8);
            counts_unaff[rc * info.num_counts_per_combination + c] = count / 8;
        }
    }
}

uint8_t* set_genotypes_masks(int order, uint8_t **genotypes, int num_combinations, masks_info info) {
    /* 
     * Structure: Genotypes of a SNP in each 'row'
     * 
     * SNP(0) - Mask genotype 0 (all samples)
     * SNP(0) - Mask genotype 1 (all samples)
     * SNP(0) - Mask genotype 2 (all samples)
     * 
     * SNP(1) - Mask genotype 0 (all samples)
     * SNP(1) - Mask genotype 1 (all samples)
     * SNP(1) - Mask genotype 2 (all samples)
     * 
     * ...
     * 
     * SNP(order-1) - Mask genotype 0 (all samples)
     * SNP(order-1) - Mask genotype 1 (all samples)
     * SNP(order-1) - Mask genotype 2 (all samples)
     */
    __m128i reference_genotype; // The genotype to compare for generating a mask (of the form {0 0 0 0 ... }, {1 1 1 1 ... })
    __m128i input_genotypes;    // Genotypes from the input dataset
    __m128i mask;               // Comparison between the reference genotype and input genotypes
    
    uint8_t *masks = info.masks;
    
    for (int c = 0; c < num_combinations; c++) {
        masks = info.masks + c * info.num_masks;
        uint8_t **combination_genotypes = genotypes + c * order;
        
        for (int j = 0; j < order; j++) {
            for (int i = 0; i < NUM_GENOTYPES; i++) {
                reference_genotype = _mm_set1_epi8(i);

                // Set value of masks
                for (int k = 0; k < info.num_samples_per_mask; k += 16) {
                    input_genotypes = _mm_load_si128(combination_genotypes[j] + k);
                    mask = _mm_cmpeq_epi8(input_genotypes, reference_genotype);
                    _mm_store_si128(masks + j * NUM_GENOTYPES * (info.num_samples_per_mask) + i * (info.num_samples_per_mask) + k, mask);
                }

                // Set padding with zeroes
                memset(masks + j * NUM_GENOTYPES * (info.num_samples_per_mask) + i * (info.num_samples_per_mask) + info.num_affected,
                    0, info.num_affected_with_padding - info.num_affected);
                memset(masks + j * NUM_GENOTYPES * (info.num_samples_per_mask) + i * (info.num_samples_per_mask) + 
                    info.num_affected_with_padding + info.num_unaffected,
                    0, info.num_unaffected_with_padding - info.num_unaffected);
            }
        }
    }
    
    masks = info.masks;
    
    return masks;
}

void masks_info_init(int order, int num_combinations_in_a_row, int num_affected, int num_unaffected, masks_info *info) {
    info->num_affected = num_affected;
    info->num_unaffected = num_unaffected;
    info->num_affected_with_padding = 16 * (int) ceil(((double) num_affected) / 16);
    info->num_unaffected_with_padding = 16 * (int) ceil(((double) num_unaffected) / 16);
    info->num_combinations_in_a_row = num_combinations_in_a_row;
    info->num_counts_per_combination = pow(NUM_GENOTYPES, order);
    info->num_samples_per_mask = info->num_affected_with_padding + info->num_unaffected_with_padding;
    info->num_masks = NUM_GENOTYPES * order * info->num_samples_per_mask;
    info->masks = _mm_malloc(info->num_combinations_in_a_row * info->num_masks * sizeof(uint8_t), 16);
    
    assert(info->masks);
    assert(info->num_affected_with_padding);
    assert(info->num_unaffected_with_padding);
}


/* **************************
 *         High risk        *
 * **************************/

int* choose_high_risk_combinations2(unsigned int* counts_aff, unsigned int* counts_unaff, 
                                   unsigned int num_combinations, unsigned int num_counts_per_combination,
                                   unsigned int num_affected, unsigned int num_unaffected, 
                                   unsigned int *num_risky, void** aux_ret, 
                                   int* (*test_func)(unsigned int, unsigned int, unsigned int, unsigned int, unsigned int, void **)) {
    int num_counts = num_combinations * num_counts_per_combination;
    
    void *test_return_values = NULL;
    // Check high risk for all combinations
    int *is_high_risk = test_func(counts_aff, counts_unaff, num_counts, num_affected, num_unaffected, &test_return_values);
        
    int *risky = malloc (num_counts * sizeof(int)); // Put all risky indexes together
    
    int total_risky = 0;
    for (int i = 0; i < num_counts; i++) {
        if (is_high_risk[i]) {
            int c = i / num_counts_per_combination;
            int idx = i % num_counts_per_combination;
            
            risky[total_risky] = idx;
            num_risky[c]++;
            total_risky++;
        }
    }
    
    free(is_high_risk);
    
    return risky;
}

int* choose_high_risk_combinations(unsigned int* counts_aff, unsigned int* counts_unaff, unsigned int num_counts, 
                                   unsigned int num_affected, unsigned int num_unaffected, 
                                   unsigned int *num_risky, void** aux_ret, 
                                   bool (*test_func)(unsigned int, unsigned int, unsigned int, unsigned int, void **)) {
    int *risky = malloc (num_counts * sizeof(int));
    *num_risky = 0;
    
    for (int i = 0; i < num_counts; i++) {
        void *test_return_values = NULL;
        bool is_high_risk = test_func(counts_aff[i], counts_unaff[i], num_affected, num_unaffected, &test_return_values);
        
        if (is_high_risk) {
            risky[*num_risky] = i;
            if (test_return_values) { *aux_ret = test_return_values; }
            (*num_risky)++;
        }
    }
    
    return risky;
}

risky_combination* risky_combination_new(int order, int comb[order], uint8_t** possible_genotypes_combinations, 
                                         int num_risky, int* risky_idx, void *aux_info) {
    risky_combination *risky = malloc(sizeof(risky_combination));
    risky->order = order;
    risky->combination = malloc(order * sizeof(int));
    risky->genotypes = malloc(pow(NUM_GENOTYPES, order) * order * sizeof(uint8_t)); // Maximum possible
    risky->num_risky_genotypes = num_risky;
    risky->auxiliary_info = aux_info; // TODO improvement: set this using a method-dependant (MDR, MB-MDR) function
    
    memcpy(risky->combination, comb, order * sizeof(int));
    
    for (int i = 0; i < num_risky; i++) {
        memcpy(risky->genotypes + (order * i), possible_genotypes_combinations[risky_idx[i]], order * sizeof(uint8_t));
    }
    
    return risky;
}

risky_combination* risky_combination_copy(int order, int comb[order], uint8_t** possible_genotypes_combinations, 
                                          int num_risky, int* risky_idx, void *aux_info, risky_combination* risky) {
    assert(risky);
    risky->num_risky_genotypes = num_risky;
    risky->auxiliary_info = aux_info; // TODO improvement: set this using a method-dependant (MDR, MB-MDR) function
    
    memcpy(risky->combination, comb, order * sizeof(int));
    for (int i = 0; i < num_risky; i++) {
        memcpy(risky->genotypes + (order * i), possible_genotypes_combinations[risky_idx[i]], order * sizeof(uint8_t));
    }
    
    return risky;
}

void risky_combination_free(risky_combination* combination) {
    free(combination->combination);
    free(combination->genotypes);
    free(combination);
}


/* **************************
 *  Evaluation and ranking  *
 * **************************/

void confusion_matrix(int order, risky_combination *combination, masks_info info, uint8_t **genotypes, unsigned int *matrix) {
    int num_samples = info.num_samples_per_mask;
    uint8_t confusion_masks[combination->num_risky_genotypes * num_samples];
    memset(confusion_masks, 0, combination->num_risky_genotypes * num_samples * sizeof(uint8_t));
    
    __m128i comb_genotypes;     // The genotype to compare for generating a mask (of the form {0 0 0 0 ... }, {1 1 1 1 ... })
    __m128i input_genotypes;    // Genotypes from the input dataset
    __m128i mask;               // Comparison between the reference genotype and input genotypes
    
    
    // Check whether the input genotypes can be combined in any of the risky combinations
    for (int i = 0; i < combination->num_risky_genotypes; i++) {
        // First SNP in the combination
        comb_genotypes = _mm_set1_epi8(combination->genotypes[i * order]);
        
        for (int k = 0; k < info.num_samples_per_mask; k += 16) {
            input_genotypes = _mm_load_si128(genotypes[0] + k);
            mask = _mm_cmpeq_epi8(input_genotypes, comb_genotypes);
            _mm_store_si128(confusion_masks + i * num_samples + k, mask);
        }
        
        // Next SNPs in the combination
        for (int j = 1; j < order; j++) {
            comb_genotypes = _mm_set1_epi8(combination->genotypes[i * order + j]);
            
            for (int k = 0; k < info.num_samples_per_mask; k += 16) {
                input_genotypes = _mm_load_si128(genotypes[j] + k);
                mask = _mm_load_si128(confusion_masks + i * num_samples + k);
                mask = _mm_and_si128(mask, _mm_cmpeq_epi8(input_genotypes, comb_genotypes));
                _mm_store_si128(confusion_masks + i * num_samples + k, mask);
            }
        }
    }
    
/*
    printf("confusion masks sse = {\n");
    for (int j = 0; j < combination->num_risky_genotypes; j++) {
        printf(" comb %d = { ", j);
        for (int k = 0; k < num_samples; k++) {
            printf("%03d ", confusion_masks[j * num_samples + k]);
        }
        printf("}\n");
    }
    printf("}\n");
*/
   
    uint8_t final_masks[num_samples];
    __m128i final_or, other_mask;
    
    for (int k = 0; k < num_samples; k += 16) {
        final_or = _mm_load_si128(confusion_masks + k); // TODO first mask
        
        // Merge all positives (1) and negatives (0)
        for (int j = 1; j < combination->num_risky_genotypes; j++) {
            other_mask = _mm_load_si128(confusion_masks + j * num_samples + k);
            final_or = _mm_or_si128(final_or, other_mask);
        }
        
        _mm_store_si128(final_masks + k, final_or);
    }
    
/*
    printf("final masks sse = {\n");
    for (int k = 0; k < num_samples; k++) {
        printf("%d ", final_masks[k]);
    }
    printf("}\n");
*/
   
    // Get the counts (popcount is the number of 1s -> popcount / 8 is the number of positives)
    int popcount0 = 0, popcount1 = 0;
    __m128i snp_and;
    
    memset(final_masks + info.num_affected, 0, info.num_affected_with_padding - info.num_affected);
    memset(final_masks + info.num_affected_with_padding + info.num_unaffected, 0, info.num_unaffected_with_padding - info.num_unaffected);
    
    for (int k = 0; k < info.num_affected; k += 16) {
        snp_and = _mm_load_si128(final_masks + k);
        popcount0 += _mm_popcnt_u64(_mm_extract_epi64(snp_and, 0)) + 
                     _mm_popcnt_u64(_mm_extract_epi64(snp_and, 1));
    }
    
    for (int k = 0; k < info.num_unaffected; k += 16) {
        snp_and = _mm_load_si128(final_masks + info.num_affected_with_padding + k);
        popcount1 += _mm_popcnt_u64(_mm_extract_epi64(snp_and, 0)) + 
                     _mm_popcnt_u64(_mm_extract_epi64(snp_and, 1));
    }
    
    matrix[0] = popcount0 / 8;
    matrix[1] = info.num_affected - popcount0 / 8;
    matrix[2] = popcount1 / 8;
    matrix[3] = info.num_unaffected - popcount1 / 8;
    
/*
    assert(matrix[0] + matrix[1] + matrix[2] + matrix[3] == info.num_affected + info.num_unaffected);
*/
}


double evaluate_model(unsigned int *confusion_matrix, enum eval_function function) {
    double TP = confusion_matrix[0], FN = confusion_matrix[1], FP = confusion_matrix[2], TN = confusion_matrix[3];
    
    if (!function) {
        function = BA;
    }
    
    switch(function) {
        case CA:
            return (TP + TN) / (TP + FN + TN + FP);
        case BA:
            return ((TP / (TP + FN)) + (TN / (TN + FP))) / 2;
        case GAMMA:
            return (TP * TN - FP * FN) / (TP * TN + FP * FN);
        case TAU_B:
            return (TP * TN - FP * FN) / sqrt((TP + FN) * (TN + FP) * (TP + FP) * (TN + FN));
    }
}

