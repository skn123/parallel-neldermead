/*
 * DistParNelderMead.cpp
 *
 * Implements MPI based distributed memory parallel NelderMead simplex method.
 *
 *  Created on: May 10, 2011
 *      Author: kyleklein
 */
#include "DistParNelderMead.hpp"
#include <mpi.h>
#include <iostream>
#include "string.h"
#include <algorithm>

DistParNelderMead::DistParNelderMead(double *guess, double step, int dimension,
		double (*obj_function)(double *vector, int dimension), int rank, int size,
		int points_per_iter) {
	init(guess, step, dimension, obj_function, rank, size, points_per_iter);
}

DistParNelderMead::DistParNelderMead(int dimension,
		double (*obj_function)(double *vector, int dimension), int rank, int size,
		int points_per_iter) {
	double *guess = new double[dimension];
	for (int i = 0; i < dimension; i++)
		guess[i] = 1.0;
	init(guess, 1.0, dimension, obj_function, rank, size, points_per_iter);
	delete guess;
}

void DistParNelderMead::init(double *guess, double step, int dimension,
		double (*obj_function)(double *vector, int dimension), int rank, int size,
		int points_per_iter) {

	/* Determine how many points are on the given processor, and their global
	 * indices. Based off this index update with the provided step size. */
	points_on_proc = (dimension + 1) / size;
	if ((dimension + 1) % size < rank)
		points_on_proc++;
	int globalFirstIndex = rank * ((dimension + 1) / size)
			+ std::min((dimension + 1) % size, rank);

	indices = new int[points_on_proc];
	for (int i = 0; i < points_on_proc; i++) {
		indices[i] = i;
	}
	this->simplex = new double[dimension * points_on_proc];
	for (int i = 0; i < points_on_proc; i++) {
		for (int j = 0; j < dimension; j++) {
			SIMPLEX(i, j) = guess[j];
			if (globalFirstIndex + i == j + 1)
				SIMPLEX(i, j) += step;
		}
	}
	this->dimension = dimension;
	this->obj_function = obj_function;
	this->rank = rank;
	this->size = size;
	this->points_per_iter = points_per_iter;
	M = new double[dimension];
	obj_function_results = new double[points_on_proc];
	AR = new double[dimension];
	AE = new double[dimension];
	AC = new double[dimension];
	updated = false;
	alpha = ALPHA;
	beta = BETA;
	gamma = GAMMA;
	tau = TAU;
}

DistParNelderMead::~DistParNelderMead() {
	delete indices;
	delete simplex;
	delete M;
	delete obj_function_results;
	delete AR;
	delete AE;
	delete AC;
}

double* DistParNelderMead::solve(int max_iterations) {
	//Compute objective function
	for (int i = 0; i < points_on_proc; i++) {
		obj_function_results[i] = obj_function(&SIMPLEX(i, 0), dimension);
	}
	sort_simplex(); //Sort the simplex
	MPI_Allreduce(&(obj_function_results[indices[0]]), &best, 1, MPI_DOUBLE,
			MPI_MIN, MPI_COMM_WORLD);

	int iter = 0;
	while (best > 1e-6 && (max_iterations <= 0 || iter * size < max_iterations)) {
		current_point = points_on_proc - (iter % points_per_iter) - 1;

		if (iter % points_per_iter == 0) {
			centroid();
		}
		reflection(); //Compute reflection
		fAR = obj_function(AR, dimension); //Evaluate reflection
		//Case 1
		if (fAR < best) {
			expansion();
			fAE = obj_function(AE, dimension); //Evaluate expansion
			//If expansion is better, use that
			if (fAE < best) {
				//std::cout << "expansion best: " << fAE << "\n";
				update(AE, current_point);
				obj_function_results[indices[current_point]] = fAE;
			} else { //otherwise use reflection
				//std::cout << "reflection best\n";
				update(AR, current_point);
				obj_function_results[indices[current_point]] = fAR;
			}
			//Case 2
		} else if (fAR < obj_function_results[indices[current_point - 1]]) {
			//std::cout << "AR better than next worst\n";
			update(AR, current_point);
			obj_function_results[indices[current_point]] = fAR;
			//Case 3
		} else {
			contraction();
			fAC = obj_function(AC, dimension); //Evaluate contraction
			//If Contraction is better, use it
			if (fAC < std::min(obj_function_results[indices[current_point]], fAR)) {
				//std::cout << "contraction better than worst\n";
				update(AC, current_point);
				obj_function_results[indices[current_point]] = fAC;
			} else { //Otherwise, minimize
				if (fAR < obj_function_results[indices[current_point]]) {
					//std::cout << "reflection better than worst\n";
					memmove(&SIMPLEX(current_point, 0), AR, dimension * sizeof(double));
					obj_function_results[indices[current_point]] = fAR;
				} //Else, not updated so don't bother
			}

		}
		if ((iter % points_per_iter) == points_per_iter - 1) {
			int global_updated = 0;
			MPI_Allreduce(&updated, &global_updated, 1, MPI_INT, MPI_SUM,
					MPI_COMM_WORLD);
			if (!global_updated) { //not one processor had an update, minimize
				minimize();
				//Re-eval all of the points
				for (int i = 0; i < points_on_proc; i++) {
					obj_function_results[indices[i]] = obj_function(&SIMPLEX(i, 0),
							dimension);
				}
			}
			sort_simplex(); //Sort the simplex
			//Find the new best
			best = obj_function_results[indices[0]];
			//Update global min on all processors
			MPI_Allreduce(&(obj_function_results[indices[0]]), &best, 1, MPI_DOUBLE,
					MPI_MIN, MPI_COMM_WORLD);
			updated = 0;
		}
		/*if (iter * size % 500 == 0 && rank == 0) {
				std::cout << iter << " " " " << best << std::endl;
			}*/
		iter++;
	}
	if (rank == 0)
		std::cout << "Total Iterations: " << size * (iter) << std::endl;
	double *answer = new double[dimension];
	global_best(answer);
	return answer;
}

void DistParNelderMead::update(double *vector, int index) {
	if (!updated) { //only need to check if not already updated
		for (int i = 0; i < dimension; i++) {
			if (vector[i] != SIMPLEX(index, i)) {
				updated = 1;
				break;
			}
		}
	}
	if (updated) { //might be a new vector, copy it in
		memmove(&SIMPLEX(index, 0), vector, dimension * sizeof(double));
	}
}

void DistParNelderMead::centroid() {
	for (int i = 0; i < dimension; i++)
		M[i] = 0.0;
	for (int i = 0; i < points_on_proc - points_per_iter; i++) {
		for (int j = 0; j < dimension; j++) {
			M[j] += SIMPLEX(i, j);
			//Divide after. Possible overflow for large obj function values!
		}
	}
	for (int i = 0; i < dimension; i++) {
		M[i] /= (dimension + 1); //Divide from earlier, then compute
	}
	//Reduce into AR, then swap pointers.
	MPI_Allreduce(M, AR, dimension, MPI_DOUBLE, MPI_SUM, MPI_COMM_WORLD);
	double *swap = M;
	M = AR;
	AR = swap;
}

void DistParNelderMead::reflection() {
	for (int i = 0; i < dimension; i++) {
		AR[i] = M[i] + alpha * (M[i] - SIMPLEX(current_point, i));
	}
}

void DistParNelderMead::expansion() {
	for (int i = 0; i < dimension; i++) {
		AE[i] = AR[i] + gamma * (AR[i] - M[i]);
	}
}

void DistParNelderMead::contraction() {
	double *ATilda;
	if (fAR < obj_function_results[indices[current_point]]) {
		ATilda = AR;
	} else {
		ATilda = &SIMPLEX(current_point, 0);
	}
	for (int i = 0; i < dimension; i++) {
		AC[i] = M[i] + beta * (ATilda[i] - M[i]);
	}
}

void DistParNelderMead::global_best(double *global_best) {
	struct {
		double val;
		int rank;
	} myBest, global_bestVal;

	myBest.val = obj_function_results[indices[0]];
	myBest.rank = rank;
	MPI_Allreduce(&myBest, &global_bestVal, 1, MPI_DOUBLE_INT, MPI_MINLOC,
			MPI_COMM_WORLD);
	if (rank == global_bestVal.rank) {
		memmove(global_best, &SIMPLEX(0, 0), dimension * sizeof(double));
	}
	MPI_Bcast(global_best, dimension, MPI_DOUBLE, global_bestVal.rank,
			MPI_COMM_WORLD);

}

void DistParNelderMead::minimize() {
	double *global_bestPoint = AC; // AC is currently unusued unused memory
	global_best(global_bestPoint);
	for (int i = 0; i < points_on_proc; i++) {
		daxpy(&SIMPLEX(i, 0), tau, global_bestPoint, (1.0 - tau), &SIMPLEX(i, 0),
				dimension);
	}

}

// result = scalar1*a + scalar2*b
void DistParNelderMead::daxpy(double *result, double scalar1, double *a,
		double scalar2, double *b, int length) {
	for (int i = 0; i < length; i++) {
		result[i] = scalar1 * a[i] + scalar2 * b[i];
	}
}

//Debugging purposes
void DistParNelderMead::print_simplex() {
	for (int i = 0; i < points_on_proc; i++) {
		for (int j = 0; j < dimension; j++) {
			std::cout << SIMPLEX(i, j) << " ";
		}
		std::cout << std::endl;
	}
	std::cout << std::endl;
}

void DistParNelderMead::sort_simplex() {
	std::sort(indices, indices + points_on_proc,
			IndexSorter(obj_function_results));
}