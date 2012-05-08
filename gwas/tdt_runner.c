#include "tdt_runner.h"

int permute = 0;

int run_tdt_test(global_options_data_t* global_options_data, gwas_options_data_t* options_data) {
    list_t *read_list = (list_t*) malloc(sizeof(list_t));
    list_init("batches", 1, options_data->max_batches, read_list);
    list_t *output_list = (list_t*) malloc (sizeof(list_t));
    list_init("output", options_data->num_threads, MIN(10, options_data->max_batches) * options_data->batch_size, output_list);

    int ret_code = 0;
    double start, stop, total;
    vcf_file_t *file = vcf_open(global_options_data->vcf_filename);
    ped_file_t *ped_file = ped_open(global_options_data->ped_filename);
    size_t output_directory_len = strlen(global_options_data->output_directory);
    
    LOG_INFO("About to read PED file...\n");
    // Read PED file before doing any proccessing
    ret_code = ped_read(ped_file);
    if (ret_code != 0) {
        LOG_FATAL_F("Can't read PED file: %s\n", ped_file->filename);
    }
    
    // Try to create the directory where the output files will be stored
    ret_code = create_directory(global_options_data->output_directory);
    if (ret_code != 0 && errno != EEXIST) {
        LOG_FATAL_F("Can't create output directory: %s\n", global_options_data->output_directory);
    }
    
    LOG_INFO("About to perform TDT test...\n");

#pragma omp parallel sections private(start, stop, total)
    {
#pragma omp section
        {
            LOG_DEBUG_F("Thread %d reads the VCF file\n", omp_get_thread_num());
            // Reading
            start = omp_get_wtime();

            ret_code = vcf_read_batches(read_list, options_data->batch_size, file, 1);

            stop = omp_get_wtime();
            total = stop - start;

            if (ret_code) {
                LOG_FATAL_F("Error %d while reading the file %s\n", ret_code, file->filename);
            }

            LOG_INFO_F("[%dR] Time elapsed = %f s\n", omp_get_thread_num(), total);
            LOG_INFO_F("[%dR] Time elapsed = %e ms\n", omp_get_thread_num(), total*1000);

            list_decr_writers(read_list);
        }

#pragma omp section
        {
            // Enable nested parallelism and set the number of threads the user has chosen
            omp_set_nested(1);
            omp_set_num_threads(options_data->num_threads);
            
            LOG_DEBUG_F("Thread %d processes data\n", omp_get_thread_num());
            
            cp_hashtable *sample_ids = NULL;
            
            // Create chain of filters for the VCF file
            filter_t **filters = NULL;
            int num_filters = 0;
            if (options_data->chain != NULL) {
                filters = sort_filter_chain(options_data->chain, &num_filters);
            }
    
            
            start = omp_get_wtime();

            int i = 0;
            list_item_t *item = NULL;
            while ((item = list_remove_item(read_list)) != NULL) {
                // In the first iteration, create map to associate the position of individuals in the list of samples defined in the VCF file
                if (i == 0) {
                    sample_ids = associate_samples_and_positions(file);
                }
                
                vcf_batch_t *batch = (vcf_batch_t*) item->data_p;
                list_t *input_records = batch;
                list_t *passed_records = NULL, *failed_records = NULL;

                if (i % 20 == 0) {
                    LOG_INFO_F("Batch %d reached by thread %d - %zu/%zu records \n", 
                            i, omp_get_thread_num(),
                            batch->length, batch->max_length);
                }

                if (filters == NULL) {
                    passed_records = input_records;
                } else {
                    failed_records = (list_t*) malloc(sizeof(list_t));
                    list_init("failed_records", 1, INT_MAX, failed_records);
                    passed_records = run_filter_chain(input_records, failed_records, filters, num_filters);
                }

                // Launch TDT test over records that passed the filters
                if (passed_records->length > 0) {
                    // Divide the list of passed records in ranges of size defined in config file
                    int max_chunk_size = 1000;  // TODO define dynamically
                    int num_chunks;
                    list_item_t **chunk_starts = create_chunks(passed_records, max_chunk_size, &num_chunks);
                    
                    // OpenMP: Launch a thread for each range
                    #pragma omp parallel for
                    for (int j = 0; j < num_chunks; j++) {
                        LOG_DEBUG_F("[%d] Test execution\n", omp_get_thread_num());
                        ret_code = tdt_test(ped_file, chunk_starts[j], max_chunk_size, sample_ids, output_list);
                    }
                    free(chunk_starts);
                    
                    LOG_INFO_F("*** %dth TDT execution finished\n", i);
                    
                    if (ret_code) {
//                         LOG_FATAL_F("TDT error: %s\n", get_last_http_error(ret_code));
                        break;
                    }
                }
                
                // Free items in both lists (not their internal data)
                if (passed_records != input_records) {
                    LOG_DEBUG_F("[Batch %d] %zu passed records\n", i, passed_records->length);
                    list_free(passed_records, NULL);
                }
                if (failed_records) {
                    LOG_DEBUG_F("[Batch %d] %zu failed records\n", i, failed_records->length);
                    list_free(failed_records, NULL);
                }
                // Free batch and its contents
                vcf_batch_free(item->data_p);
                list_item_free(item);
                
                i++;
            }

            stop = omp_get_wtime();

            total = stop - start;

            LOG_INFO_F("[%d] Time elapsed = %f s\n", omp_get_thread_num(), total);
            LOG_INFO_F("[%d] Time elapsed = %e ms\n", omp_get_thread_num(), total*1000);

            // Free resources
            if (sample_ids) { cp_hashtable_destroy(sample_ids); }
            
            // Free filters
            for (i = 0; i < num_filters; i++) {
                filter_t *filter = filters[i];
                filter->free_func(filter);
            }
            free(filters);
            
            // Decrease list writers count
            for (i = 0; i < options_data->num_threads; i++) {
                list_decr_writers(output_list);
            }
        }

#pragma omp section
        {
            // Thread which writes the results to the output file
            FILE *fd = NULL;    // TODO check if output file is defined
            char *path = NULL, *filename = NULL;
            size_t filename_len = 0;
            
            // Set whole path to the output file
            if (global_options_data->output_filename != NULL && 
                strlen(global_options_data->output_filename) > 0) {
                filename_len = strlen(global_options_data->output_filename);
                filename = global_options_data->output_filename;
            } else {
                filename_len = strlen("hpg-variant.tdt");
                filename = (char*) calloc (filename_len+1, sizeof(char));
                strncpy(filename, "hpg-variant.tdt", filename_len);
            }
            path = (char*) calloc ((output_directory_len + filename_len + 1), sizeof(char));
            strncat(path, global_options_data->output_directory, output_directory_len);
            strncat(path, filename, filename_len);
            fd = fopen(path, "w");
            
            LOG_INFO_F("TDT output filename = %s\n", path);
            free(filename);
            free(path);
            
            // Write data: header + one line per variant
            list_item_t* item = NULL;
            tdt_result_t *result;
            fprintf(fd, " CHR          BP       A1      A2       T       U          OR            CHISQ            P\n");
            while ((item = list_remove_item(output_list)) != NULL) {
                result = item->data_p;
                
                fprintf(fd, "%s\t%12ld\t%s\t%s\t%d\t%d\t%8f\t%6f\n",//\t%f\n", 
                       result->chromosome, result->position, result->reference, result->alternate, 
                       result->t1, result->t2, result->o_range, result->chi_square);//p_value);
                
                tdt_result_free(result);
                list_item_free(item);
            }
            
            fclose(fd);
        }
    }
    
    free(read_list);
    free(output_list);
    vcf_close(file);
    // TODO delete conflicts among frees
    ped_close(ped_file, 0);
        
    return ret_code;
}


int tdt_test(ped_file_t *ped_file, list_item_t *variants, int num_variants, cp_hashtable *sample_ids, list_t *output_list) {
    int ret_code = 0;
    int tid = omp_get_thread_num();
    cp_hashtable *families = ped_file->families;
    int num_families = get_num_families(ped_file);
    int num_samples = cp_hashtable_count(sample_ids);
    
    tdt_result_t *result;
//     tdt_result_t *result = (tdt_result_t*) calloc (1, sizeof(tdt_result_t));
    char **sample_data;// = (char**) calloc (num_samples, sizeof(char*));
    
    
    int father_allele1, father_allele2;
    int mother_allele1, mother_allele2;
    int child_allele1, child_allele2;

    ///////////////////////////////////
    // Perform analysis for each variant

    list_item_t *cur_variant = variants;
    // TODO chunks in the same way as in hpg-variant/effect
    for (int i = 0; i < num_variants && cur_variant != NULL; i++) {
        vcf_record_t *record = (vcf_record_t*) cur_variant->data_p;
        LOG_INFO_F("[%d] Checking variant %s:%ld\n", tid, record->chromosome, record->position);
        
    //         // Adaptive permutation, skip this SNP?
    //         if (par::adaptive_perm && (!perm.snp_test[variant])) {
    //             continue;
    //         }

        // TODO implement arraylist in order to avoid this conversion
        sample_data = (char**) list_to_array(record->samples);
    
        // Transmission counts
        int t1 = 0;
        int t2 = 0;
        
        
        // Count over families
        char **keys = (char**) cp_hashtable_get_keys(families);
        family_t *family;
        for (int f = 0; f < num_families; f++) {
            family = cp_hashtable_get(families, keys[f]);
            individual_t *father = family->father;
            individual_t *mother = family->mother;
            cp_list *children = family->children;

            LOG_DEBUG_F("[%d] Checking suitability of family %s\n", tid, family->id);
            
            if (father == NULL || mother == NULL) {
                continue;
            }
//             if ( !family[f]->TDT ) continue;

            int *father_pos = cp_hashtable_get(sample_ids, father->id);
            if (father_pos != NULL) {
                LOG_DEBUG_F("[%d] Father %s is in position %d\n", tid, father->id, *father_pos);
            } else {
                LOG_DEBUG_F("[%d] Father %s is not positioned\n", tid, father->id);
                continue;
            }
            
            int *mother_pos = cp_hashtable_get(sample_ids, mother->id);
            if (mother_pos != NULL) {
                LOG_DEBUG_F("[%d] Mother %s is in position %d\n", tid, mother->id, *mother_pos);
            } else {
                LOG_DEBUG_F("[%d] Mother %s is not positioned\n", tid, mother->id);
                continue;
            }
            
            char *father_sample = sample_data[*father_pos];
            char *mother_sample = sample_data[*mother_pos];
            
            LOG_DEBUG_F("[%d] Samples: Father = %s\tMother = %s\n", tid, father_sample, mother_sample);
            // If any parent's alleles can't be read or is missing, go to next family
            if (get_alleles(father_sample, &father_allele1, &father_allele2) ||
                get_alleles(mother_sample, &mother_allele1, &mother_allele2)) {
                    continue;
            }
            
            LOG_DEBUG_F("[%d] Alleles: Father = %d/%d\tMother = %d/%d\n", tid, father_allele1, father_allele2, mother_allele1, mother_allele2);
            // We need two genotyped parents, with at least one het
            if (father_allele1 == father_allele2 && mother_allele1 == mother_allele2) {
                continue;
            }
            
            if ((father_allele1 && !father_allele2) || (mother_allele1 && !mother_allele2)) {
                continue;
            }

            LOG_DEBUG_F("[%d] Proceeding to analyse family %s...\n", tid, family->id);

            
            int trA = 0;  // transmitted allele from first het parent
            int unA = 0;  // untransmitted allele from first het parent
            
            int trB = 0;  // transmitted allele from second het parent
            int unB = 0;  // untransmitted allele from second het parent
            
            // Consider all offspring in nuclear family
            cp_list_iterator *children_iterator = cp_list_create_iterator(family->children, COLLECTION_LOCK_READ);
            individual_t *child = NULL;
            while ((child = cp_list_iterator_next(children_iterator)) != NULL) {
                // Only consider affected children
                // TODO Accept non-default specification using 0 as unaffected and 1 as affected
//                 printf("[%d] Child phenotype = %f\n", child->phenotype);
                if (child->phenotype != 2.0f) { continue; }
                
                int *child_pos = cp_hashtable_get(sample_ids, child->id);
                if (child_pos != NULL) {
                    LOG_DEBUG_F("[%d] Child %s is in position %d\n", tid, child->id, *child_pos);
                } else {
                    LOG_DEBUG_F("[%d] Child %s is not positioned\n", tid, child->id);
                    continue;
                }
                
                char *child_sample = sample_data[*child_pos];
                LOG_DEBUG_F("[%d] Samples: Child = %s\n", tid, child_sample);
                
                if (get_alleles(child_sample, &child_allele1, &child_allele2)) {
                    continue;
                }
                
                // Skip if offspring has missing genotype
                if (child_allele1 && !child_allele2) { continue; }
                
                // We've now established: no missing genotypes
                // and at least one heterozygous parent

                // Kid is 00

                if (!child_allele1 && !child_allele2) {
                    if ( ( (!father_allele1) && father_allele2 ) && 
                        ( (!mother_allele1) && mother_allele2 ) )
                    { trA=1; unA=2; trB=1; unB=2; }
                    else 
                    { trA=1; unA=2; } 
                }
                else if ( (!child_allele1) && child_allele2 )  // Kid is 01
                {
                    // het dad
                    if (father_allele1 != father_allele2 )
                    {
                        // het mum
                        if ( mother_allele1 != mother_allele2 )
                    { trA=1; trB=2; unA=2; unB=1; }
                        else if ( !mother_allele1 ) 
                    { trA=2; unA=1; }
                        else { trA=1; unA=2; }
                    }
                    else if ( !father_allele1 ) 
                    {
                        trA=2; unA=1; 
                    }           
                    else
                    {
                        trA=1; unA=2;
                    }
                }
                else // kid is 1/1
                {
                    
                    if ( ( (!father_allele1) && father_allele2 ) && 
                        ( (!mother_allele1) && mother_allele2 ) )
                    { trA=2; unA=1; trB=2; unB=1; }
                    else 
                    { 
                        trA=2; unA=1;
                    }
                }
                
                // We have now populated trA (first transmission) 
                // and possibly trB also 
                
                ////////////////////////////////////////
                // Permutation? 50:50 flip (precomputed)
                
                if (permute) {
//                     if (flipA[f])
//                     {
                    int t = trA;
                    trA = unA;
                    unA = t;
                    
                    t = trB;
                    trB = unB;
                    unB = t;
//                     }
                }
                
                // Increment transmission counts
                if (trA==1) { t1++; }
                if (trB==1) { t1++; }
                if (trA==2) { t2++; }
                if (trB==2) { t2++; }
                
                LOG_DEBUG_F("TDT\t%s %s : %d %d - %d %d - %d %d - F %d/%d - M %d/%d - C %d/%d\n", 
                            record->id, family->id, trA, unA, trB, unB, t1, t2, 
                            father_allele1, father_allele2, mother_allele1, mother_allele2, child_allele1, child_allele2);
            } // next offspring in family
            cp_list_iterator_destroy(children_iterator);
        
        }  // next nuclear family

        /////////////////////////////
        // Finished counting: now compute
        // the statistics
        
        double tdt_chisq, par_chisq, com_chisq;
        tdt_chisq = par_chisq = com_chisq = -1;
        
        // Basic TDT test
        if (t1+t2 > 0) {
            tdt_chisq = ((double) ((t1-t2) * (t1-t2))) / (t1+t2);
        }
        
        LOG_DEBUG_F("[%d] before adding %s:%ld\n", tid, record->chromosome, record->position);
        result = tdt_result_new(record->chromosome, record->position, record->reference, record->alternate, t1, t2, tdt_chisq);
        list_item_t *output_item = list_item_new(tid, 0, result);
        list_insert_item(output_item, output_list);
        LOG_DEBUG_F("[%d] after adding %s:%ld\n", tid, record->chromosome, record->position);
        
        cur_variant = cur_variant->next_p;
    } // next variant

    free(sample_data);
    
    return ret_code;
}


tdt_result_t* tdt_result_new(char *chromosome, unsigned long int position, char *reference, char *alternate, double t1, double t2, double chi_square) {
    tdt_result_t *result = (tdt_result_t*) malloc (sizeof(tdt_result_t));
    
    result->chromosome = (char*) calloc (strlen(chromosome)+1, sizeof(char));
    strncat(result->chromosome, chromosome, strlen(chromosome));
    result->position = position;
    result->reference = (char*) calloc (strlen(reference)+1, sizeof(char));
    strncat(result->reference, reference, strlen(reference));
    result->alternate = (char*) calloc (strlen(alternate)+1, sizeof(char));
    strncat(result->alternate, alternate, strlen(alternate));
    result->t1 = t1;
    result->t2 = t2;
    result->o_range = (t2 == 0.0) ? NAN : ((double) t1/t2);
    result->chi_square = chi_square;
    
    return result;
}

void tdt_result_free(tdt_result_t* result) {
    free(result->chromosome);
    free(result->reference);
    free(result->alternate);
}


cp_hashtable* associate_samples_and_positions(vcf_file_t* file) {
    printf("** %zu sample names read\n", file->samples_names->length);
    list_t *sample_names = file->samples_names;
    cp_hashtable *sample_ids = cp_hashtable_create_by_option(COLLECTION_MODE_DEEP,
                                                             sample_names->length * 2,
                                                             cp_hash_string,
                                                             (cp_compare_fn) strcasecmp,
                                                             NULL,
                                                             NULL,
                                                             NULL,
                                                             (cp_destructor_fn) free
                                                            );
    
    list_item_t *sample_item = sample_names->first_p;
    int *index;
    char *name;
    for (int i = 0; i < sample_names->length && sample_item != NULL; i++) {
        name = sample_item->data_p;
        index = (int*) malloc (sizeof(int)); *index = i;
        cp_hashtable_put(sample_ids, (char*) sample_item->data_p, index);
        
        sample_item = sample_item->next_p;
    }
    
//     char **keys = (char**) cp_hashtable_get_keys(sample_names);
//     int num_keys = cp_hashtable_count(sample_names);
//     for (int i = 0; i < num_keys; i++) {
//         printf("%s\t%d\n", keys[i], *((int*) cp_hashtable_get(sample_ids, keys[i])));
//     }
    
    return sample_ids;
}
