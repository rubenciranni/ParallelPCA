#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <lapacke.h>
#include <cblas.h>
#include "utils/io_utils.h"
#include "utils/la_utils.h"
#include "utils/timer.h"

/* Thread data */
struct ThreadData
{
	double *thread_img;
	int thread_s;
	int thread_d;
	long rank;
	double *mean;
	double *et;
	double *st;
};

/* Global vars */
int thread_count;
int t, s;
int style;
const double DBL_MIN = -1e5;
const double DBL_MAX = 1e5;
double global_min = 0.0, global_max = 255.99;
pthread_mutex_t m;

int barrier_counter = 0;
pthread_mutex_t barrier_mutex;
pthread_cond_t barrier_cond_var;

double total_time = 0.0;

void barrier()
{
	pthread_mutex_lock(&barrier_mutex);
	barrier_counter++;
	if (barrier_counter == thread_count)
	{
		barrier_counter = 0;
		pthread_cond_broadcast(&barrier_cond_var);
	}
	else
	{
		while (pthread_cond_wait(&barrier_cond_var, &barrier_mutex) != 0)
			;
	}
	pthread_mutex_unlock(&barrier_mutex);
}

void *PCA(void *arg)
{
	// Record the start time
	double start_time, finish_time;
	barrier();
	GET_TIME(start_time);

	// Initialize thread data
	struct ThreadData *thread_data = (struct ThreadData *)arg;

	int local_s = thread_data->thread_s;
	int d = thread_data->thread_d;
	double *local_img = thread_data->thread_img;
	long rank = thread_data->rank;
	double *mean = thread_data->mean;
	double *St = thread_data->st;
	double *Et = thread_data->et;

	// Center the dataset
	double *mean_local = (double *)calloc(d, sizeof(double *));
	dataset_partial_mean(s, local_s, d, local_img, mean_local);

	pthread_mutex_lock(&m);
	accumulate_matrix(mean_local, 1, d, mean);
	pthread_mutex_unlock(&m);
	free(mean_local);

	barrier(); // wait for all the threads to accumulate on mean

	center_dataset(local_s, d, local_img, mean);

	// SVD
	double *U_local = (double *)malloc(local_s * local_s * sizeof(double));
	double *D_local = (double *)malloc(d * sizeof(double));
	double *E_localT = (double *)malloc(d * d * sizeof(double));
	SVD(local_s, d, local_img, U_local, D_local, E_localT);

	// Set singular values after t^th one to zero
	cblas_dscal(d - t, 0.0, D_local + t, 1);

	// Compute Pt_local
	SVD_reconstruct_matrix(local_s, d, U_local, D_local, E_localT, local_img);
	double *Pt_local = local_img;

	// Free SVD space
	free(U_local);
	free(D_local);
	free(E_localT);

	// Compute St
	double *St_local = (double *)calloc(d * d,sizeof(double));
	multiply_matrices(Pt_local, d, local_s, 1, Pt_local, local_s, d, 0, St_local, 0);

	pthread_mutex_lock(&m);
	accumulate_matrix(St_local, d, d, St);
	pthread_mutex_unlock(&m);
	free(St_local);

	barrier(); // wait for all the threads to accumulate on St

	// Do eigendecomposition of St in thread 0
	if (rank == 0)
	{
		double *L = (double *)malloc(d * sizeof(double));
		eigen_decomposition(d, St, L);
		double *E = St;
		reverse_matrix_columns(E, d, t, d, Et);
		free(St);
		free(L);
	}
	barrier(); // wait for thread 0

	// Obtain Pp_local (written inside local_img) by projecting Pt_local on Et (first t columns of E)
	double *Pt_localEt = (double *)malloc(local_s * t * sizeof(double));
	multiply_matrices(Pt_local, local_s, d, 0, Et, d, t, 0, Pt_localEt, 1);
	multiply_matrices(Pt_localEt, local_s, t, 0, Et, t, d, 1, local_img, 1);
	free(Pt_localEt);
	decenter_dataset(local_s, d, local_img, mean);

	// Set normalization style
	if (style == 0)
	{
		set_local_extremes(local_img, local_s, d, 0.0, 255.99);
	}
	else if (style == 1)
	{
		double local_min = DBL_MAX, local_max = DBL_MIN;
		get_local_extremes(local_img, local_s, d, &local_min, &local_max);

		pthread_mutex_lock(&m);
		if (global_min > local_min)
			global_min = local_min;
		if (global_max < local_max)
			global_max = local_max;
		pthread_mutex_unlock(&m);
		barrier();

		rescale_image(local_img, local_s, d, global_min, global_max);
	}

	// Record the finish time
	GET_TIME(finish_time);
	finish_time -= start_time;
	pthread_mutex_lock(&m);
	if (finish_time > total_time)
		total_time = finish_time;
	pthread_mutex_unlock(&m);
	barrier();

	// Print the execution time
	if (rank == 0)
	{
		printf("Total elapsed time (maximum thread execution time): %f seconds\n", total_time);
	}

	// Uncomment to output local execution times
	// printf("Thread %d > Elapsed time = %f seconds\n", rank, finish_time);

	return NULL;
}

int main(int argc, char *argv[])
{
	long thread;
	pthread_t *thread_handles;
	pthread_mutex_init(&m, NULL);
	pthread_mutex_init(&barrier_mutex, NULL);
	pthread_cond_init(&barrier_cond_var, NULL);

	// Get number of threads from the command line
	thread_count = strtol(argv[1], NULL, 10);

	// Ensure that the input filename is provided as a command-line argument
	char *input_filename;
	if (argc != 4 && argc != 5)
	{
		printf("Usage: %s <n_threads> <input_filename.jpg> <n_principal_components> <style (optional)>\n", argv[0]);
		return 1;
	}
	input_filename = argv[2];
	double *img;
	int d;
	img = read_JPEG_to_matrix(input_filename, &s, &d);
	t = atoi(argv[3]);

	if (t > d)
	{
		printf("ERROR: the number of Principal Components (%d) cannot be greater than the number of columns of the image (%d).\n\n", t, d);
		return 1;
	}

	style = 0;
	if (argc == 5)
		style = atoi(argv[4]);

	// Allocate threads
	thread_handles = (pthread_t *)malloc(thread_count * sizeof(pthread_t));

	// Allocate space for thread data
	double *mean = (double *)calloc(d, sizeof(double));
	double *St = (double *)calloc(d * d, sizeof(double));
	double *Et = (double *)malloc(d * t * sizeof(double));

	// Set threads data
	struct ThreadData data[thread_count];
	int offset = 0, info = 0;
	for (thread = 0; thread < thread_count; thread++)
	{
		data[thread].thread_img = img + offset;
		data[thread].thread_s = (thread < s % thread_count) ? s / thread_count + 1 : s / thread_count;
		data[thread].thread_d = d;
		data[thread].rank = thread;
		data[thread].mean = mean;
		data[thread].st = St;
		data[thread].et = Et;
		info = pthread_create(&thread_handles[thread], NULL, PCA, (void *)&data[thread]);
		if (info != 0)
		{
			printf("Unable to create thread %ld, returning error.\n", thread);
			return 1;
		}
		offset += (thread < s % thread_count) ? (s / thread_count + 1) * d : (s / thread_count) * d;
	}

	// Wait for the threads to finish and join
	for (thread = 0; thread < thread_count; thread++)
	{
		pthread_join(thread_handles[thread], NULL);
	}

	// Output img to JPEG
	write_matrix_to_JPEG("compressed_image.jpg", img, s, d);

	// Free memory and destroy mutexes and conditions
	free(mean);
	free(Et);
	free(img);
	free(thread_handles);
	pthread_mutex_destroy(&m);
	pthread_mutex_destroy(&barrier_mutex);
	pthread_cond_destroy(&barrier_cond_var);
	return 0;
}
