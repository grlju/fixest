/**************************************************************
* ___________________                                         *
* || OLS Functions ||                                         *
* -------------------                                         *
*                                                             *
* Author: Laurent R. Berge                                    *
*                                                             *
* Set of functions to perform OLS estimations.                *
*                                                             *
* In general, the functions are slower than BLAS ones         *
* but they have the merit of doing exacly what I want.        *
*                                                             *
**************************************************************/

#include <Rcpp.h>
#include <cmath>
#include <math.h>
#ifdef _OPENMP
    #include <omp.h>
#else
    #define omp_get_thread_num() 0
#endif
using namespace Rcpp;

// [[Rcpp::plugins(openmp)]]


std::vector<int> set_parallel_scheme(int N, int nthreads){
    // => this concerns only the parallel application on a 1-Dimensional matrix
    // takes in the nber of observations of the vector and the nber of threads
    // gives back a vector of the length the nber of threads + 1 giving the start/stop of each threads

    std::vector<int> res(nthreads + 1, 0);
    double N_rest = N;

    for(int i=0 ; i<nthreads ; ++i){
        res[i + 1] = ceil(N_rest / (nthreads - i));
        N_rest -= res[i + 1];
        res[i + 1] += res[i];
    }

    return res;
}



void invert_tri(NumericMatrix &R, int K, int nthreads = 1){

    // Startegy: we invert by bands (b) => better for parallelization

    // initialization of R prime
    for(int i=0 ; i<K ; ++i){
        for(int j=i+1 ; j<K ; ++j){
            R(j, i) = R(i, j);
        }
    }

    // b0
    for(int i=0 ; i<K ; ++i){
        R(i, i) = 1/R(i, i);
    }

    // Check for interrupts
    // number of computations is (K - b) * (b + 1) => max is (K + 1)**2 / 2
    double flop = (K + 1) * (K + 1) / 2.0;
    int iterSecond = ceil(2000000000 / flop / 5); // nber iter per 1/5 second

    for(int b=1 ; b<K ; ++b){

        if(b % iterSecond == 0){
            // Rprintf("check\n");
            R_CheckUserInterrupt();
        }

        #pragma omp parallel for num_threads(nthreads) schedule(static, 1)
        for(int i=0 ; i<K-b ; ++i){
            double numerator = 0;
            int col = i+b;
            for(int k=i+1 ; k<=col ; ++k){
                numerator -= R(k, i) * R(k, col);
            }
            R(i, col) = numerator * R(i, i);
        }
    }

}

void tproduct_tri(NumericMatrix &RRt, NumericMatrix &R, int nthreads = 1){

    int K = RRt.ncol();

    // initialization of R prime
    for(int i=0 ; i<K ; ++i){
        for(int j=i+1 ; j<K ; ++j){
            R(j, i) = R(i, j);
        }
    }

    // Check for interrupts
    // we do the same as for the invert_tri
    double flop = (K + 1) * (K + 1) / 2.0;
    int iterSecond = ceil(2000000000 / flop / 5); // nber iter per 1/5 second
    int n_iter_main = 0;

    #pragma omp parallel for num_threads(nthreads) schedule(static, 1)
    for(int i=0 ; i<K ; ++i){

        if(omp_get_thread_num() == 0 && n_iter_main % iterSecond == 0){
            // Rprintf("check\n");
            R_CheckUserInterrupt();
            ++n_iter_main;
        }

        for(int j=i ; j<K ; ++j){

            double value = 0;
            int k_start = i < j ? j : i;
            for(int k=k_start ; k<K ; ++k){
                value += R(k, j) * R(k, i);
            }
            RRt(i, j) = value;
            RRt(j, i) = value;
        }
    }

}


// [[Rcpp::export]]
List cpp_cholesky(NumericMatrix X, double tol = 1.0/100000.0/100000.0, int nthreads = 1){
    // X est symetrique, semi definie positive
    // rank-revealing on-the-fly

    List res;

    int K = X.ncol();

    NumericMatrix R(K, K);
    LogicalVector id_excl(K);
    int n_excl = 0;

    // we check for interrupt every 1s when it's the most computationnaly intensive
    // at each iteration we have K * (j+1) - j**2 - 2*j - 1 multiplications
    // max => K**2/4
    double flop = K * K / 4.0;
    int iterSecond = ceil(2000000000 / flop / 5); // nber iter per 1/5 second
    double min_norm = X(0, 0);

    for(int j=0 ; j<K ; ++j){

        if(j % iterSecond == 0){
            // Rprintf("check\n");
            R_CheckUserInterrupt();
        }

        // implicit pivoting (it's like 0-rank variables are stacked in the end)

        double R_jj = X(j, j);
        for(int k=0 ; k<j ; ++k){

            if(id_excl[k]) continue;

            R_jj -= R(k, j) * R(k, j);
        }

        if(R_jj < tol){
            n_excl++;
            id_excl[j] = true;

            // Rcout << "excluded: " << j << ", L_jj: " << L_jj << "\n";

            // Corner case, may happen:
            if(n_excl == K){
                List res;
                res["all_removed"] = true;
                return res;
            }

            continue;
        }

        if(min_norm > R_jj) min_norm = R_jj;

        R_jj = sqrt(R_jj);

        R(j, j) = R_jj;

        #pragma omp parallel for num_threads(nthreads) schedule(static, 1)
        for(int i=j+1 ; i<K ; ++i){

            double value = X(i, j);
            for(int k=0 ; k<j ; ++k){
                if(id_excl[k]) continue;

                value -= R(k, i) * R(k, j);
            }
            R(j, i) = value/R_jj;
        }
    }

    // we reconstruct the R matrix, otherwise it's just a mess to handle excluded variables on the fly
    // changes are in place

    if(n_excl > 0){
        int n_j_excl = 0;

        // The first chunk of the matrix is identical
        // we find when it starts being different
        int j_start = 0;
        for( ; !id_excl[j_start] ; ++j_start);

        for(int j=j_start ; j<K ; ++j){

            if(id_excl[j]){
                ++n_j_excl;
                continue;
            }

            int n_i_excl = 0;
            for(int i=0 ; i<=j ; ++i){
                if(id_excl[i]){
                    ++n_i_excl;
                    continue;
                }
                R(i - n_i_excl, j - n_j_excl) = R(i, j);
            }
        }

        K -= n_excl;
    }

    // Inversion of R, in place
    invert_tri(R, K, nthreads);

    NumericMatrix XtX_inv(K, K);
    tproduct_tri(XtX_inv, R, nthreads);

    res["XtX_inv"] = XtX_inv;
    res["id_excl"] = id_excl;
    res["min_norm"] = min_norm;

    return res;
}


static bool sparse_check(const NumericMatrix &X){
    // Super cheap sparsity test
    // works even for small K large N

    int N = X.nrow();
    int K = X.ncol();

    if(K < 5) return false;
    if(N < 1000 && K < 100) return false;
    if(N < 100) return false; // avoids corner cases where you have more vars than obs

    // Let's look at some rows
    // If there are factors, there should be 0s
    //
    // Let's put it differently: if there are no 0s, then it's not factors for sure

    int n0_first_row = 0;
    int n0_middle_row = 0;
    int n0_last_row = 0;

    int mid = N/2;

    for(int k=0 ; k<K ; ++k){
        n0_first_row += X(0, k) == 0;
        n0_middle_row += X(mid, k) == 0;
        n0_last_row += X(N - 1, k) == 0;
    }

    // Rcout << "1st: " << n0_first_row << ", mid: " << n0_middle_row << ", last: " << n0_last_row << "\n";

    if(n0_first_row > K/2 && n0_middle_row > K/2 && n0_last_row > K/2) return true;

    return false;
}

void set_sparse(std::vector<int> &n_j, std::vector<int> &start_j, std::vector<int> &all_i, std::vector<double> &x, const NumericMatrix &X, const NumericVector &w){

    int N = X.nrow();
    int K = X.ncol();

    bool isWeight = w.length() > 1;

    int n_obs = 0;
    for(int k=0 ; k<K ; ++k){
        for(int i=0 ; i<N ; ++i){

            if(X(i, k) != 0){
                ++n_j[k];
                all_i.push_back(i);

                if(isWeight){
                    // BEWARE: here there is no square root for w!
                    // (as opposed to the non-sparse case)
                    x.push_back(X(i, k) * w[i]);
                } else {
                    x.push_back(X(i, k));
                }
            }
        }

        n_obs += n_j[k];
        start_j[k + 1] = n_obs;
    }

}

void mp_sparse_XtX(NumericMatrix &XtX, const std::vector<int> &n_j, const std::vector<int> &start_j, const std::vector<int> &all_i, const std::vector<double> &x, const NumericMatrix &X, int nthreads){

    int K = X.ncol();

    #pragma omp parallel for num_threads(nthreads) schedule(static, 1)
    for(int j1=0 ; j1<K ; ++j1){

        int k = 0, start = 0, end = 0;

        // Upper triangle
        for(int j2=j1 ; j2<K ; ++j2){

            if(n_j[j1] < n_j[j2]){
                start = start_j[j1];
                end = start_j[j1 + 1];
                k = j2;
            } else {
                start = start_j[j2];
                end = start_j[j2 + 1];
                k = j1;
            }

            double value = 0;
            for(int index=start ; index<end ; ++index){
                value += X(all_i[index], k) * x[index];
            }

            if(value == 0) continue;

            XtX(j1, j2) = value;
            XtX(j2, j1) = value;

        }
    }
}

void mp_sparse_Xty(NumericVector &Xty, const std::vector<int> &start_j, const std::vector<int> &all_i, const std::vector<double> &x, const double *y, int nthreads){

    int K = Xty.length();

    #pragma omp parallel for num_threads(nthreads)
    for(int j=0 ; j<K ; ++j){

        int start = start_j[j];
        int end = start_j[j + 1];

        double value = 0;
        for(int index=start ; index<end ; ++index){
            value += y[all_i[index]] * x[index];
        }

        if(value == 0) continue;

        Xty[j] = value;
    }

}


// mp: mat prod
void mp_XtX(NumericMatrix &XtX, const NumericMatrix &X, const NumericMatrix &wX, int nthreads){

    int N = X.nrow();
    int K = X.ncol();

    // specific scheme for large N and K == 1
    if(K == 1){

        std::vector<double> all_values(nthreads, 0);
        std::vector<int> bounds = set_parallel_scheme(N, nthreads);

        #pragma omp parallel for num_threads(nthreads)
        for(int t=0 ; t<nthreads ; ++t){
            double val = 0;
            for(int i=bounds[t]; i<bounds[t + 1] ; ++i){
                val += X(i, 0) * wX(i, 0);
            }
            all_values[t] = val;
        }

        double value = 0;
        for(int t=0 ; t<nthreads ; ++t){
            value += all_values[t];
        }

        XtX(0, 0) = value;

    } else {
        // We use this trick to even out the load on the threads
        int nValues = K * (K + 1) / 2;
        std::vector<int> all_i, all_j;
        for(int i=0 ; i<K ; ++i){
            for(int j=i ; j<K ; ++j){
                all_i.push_back(i);
                all_j.push_back(j);
            }
        }

        #pragma omp parallel for num_threads(nthreads) schedule(static, 1)
        for(int index=0 ; index<nValues ; ++index){
            int k_row = all_i[index];
            int k_col = all_j[index];

            double val = 0;
            for(int i=0 ; i<N ; ++i){
                val += X(i, k_row) * wX(i, k_col);
            }

            XtX(k_row, k_col) = val;
            XtX(k_col, k_row) = val;
        }
    }

}

// mp: mat prod
void mp_Xty(NumericVector &Xty, const NumericMatrix &X, const double *y, int nthreads){

    int N = X.nrow();
    int K = X.ncol();

    if(K == 1){

        std::vector<double> all_values(nthreads, 0);
        std::vector<int> bounds = set_parallel_scheme(N, nthreads);

        #pragma omp parallel for num_threads(nthreads)
        for(int t=0 ; t<nthreads ; ++t){
            double val = 0;
            for(int i=bounds[t]; i<bounds[t + 1] ; ++i){
                val += X(i, 0) * y[i];
            }
            all_values[t] = val;
        }

        double value = 0;
        for(int t=0 ; t<nthreads ; ++t){
            value += all_values[t];
        }

        Xty[0] = value;
    } else  {
        #pragma omp parallel for num_threads(nthreads)
        for(int j=0 ; j<K ; ++j){
            double val = 0;
            for(int i=0 ; i<N ; ++i){
                val += X(i, j) * y[i];
            }

            Xty[j] = val;
        }
    }

}

// [[Rcpp::export]]
List cpp_sparse_products(NumericMatrix X, NumericVector w, SEXP y, bool correct_0w = false, int nthreads = 1){

    int N = X.nrow();
    int K = X.ncol();

    bool isWeight = w.length() > 1;

    bool is_y_list = TYPEOF(y) == VECSXP;

    NumericMatrix XtX(K, K);


    if(sparse_check(X) == false){
        // NOT SPARSE

        List res;

        // if(isWeight){
        //
        //     NumericMatrix wX(Rcpp::clone(X));
        //     for(int k=0 ; k<K ; ++k){
        //         for(int i=0 ; i<N ; ++i){
        //             wX(i, k) *= w[i];
        //         }
        //     }
        //
        //     // XtX
        //     mp_XtX(XtX, X, wX, nthreads);
        //
        //     // Xty
        //     mp_Xty(Xty, wX, y, nthreads);
        //
        //
        // } else {
        //     // Identique, mais pas besoin de faire une copie de X ni de y qui peuvent etre couteuses
        //
        //     // XtX
        //     mp_XtX(XtX, X, X, nthreads);
        //
        //     // Xty
        //     mp_Xty(Xty, X, y, nthreads);
        //
        // }

        NumericMatrix wX;
        if(isWeight){
           wX = Rcpp::clone(X);
            for(int k=0 ; k<K ; ++k){
                for(int i=0 ; i<N ; ++i){
                    wX(i, k) *= w[i];
                }
            }
        } else {
            // shallow copy
            wX = X;
        }

        // XtX
        mp_XtX(XtX, X, wX, nthreads);
        res["XtX"] = XtX;

        // Xty
        if(is_y_list){
            int n_vars_y = Rf_length(y);
            List Xty(n_vars_y);

            for(int v=0 ; v<n_vars_y ; ++v){
                NumericVector Xty_tmp(K);
                mp_Xty(Xty_tmp, wX, REAL(VECTOR_ELT(y, v)), nthreads);
                Xty[v] = Xty_tmp;
            }

            res["Xty"] = Xty;

        } else {
            NumericVector Xty(K);
            mp_Xty(Xty, wX, REAL(y), nthreads);
            res["Xty"] = Xty;
        }

        return res;
    }

    //
    // SPARSE case
    //

    std::vector<int> n_j(K, 0);
    std::vector<int> start_j(K + 1, 0);
    std::vector<int> all_i;
    std::vector<double> x;

    set_sparse(n_j, start_j, all_i, x, X, w);

    List res;

    // XtX
    mp_sparse_XtX(XtX, n_j, start_j, all_i, x, X, nthreads);
    res["XtX"] = XtX;

    // Xty
    if(is_y_list){
        int n_vars_y = Rf_length(y);
        List Xty(n_vars_y);

        for(int v=0 ; v<n_vars_y ; ++v){
            NumericVector Xty_tmp(K);
            mp_sparse_Xty(Xty_tmp, start_j, all_i, x, REAL(VECTOR_ELT(y, v)), nthreads);
            Xty[v] = Xty_tmp;
        }

        res["Xty"] = Xty;

    } else {
        NumericVector Xty(K);
        mp_sparse_Xty(Xty, start_j, all_i, x, REAL(y), nthreads);
        res["Xty"] = Xty;
    }

    return res;
}



// [[Rcpp::export]]
NumericMatrix cpppar_crossprod(NumericMatrix X, NumericVector w, int nthreads){

    int N = X.nrow();
    int K = X.ncol();

    bool isWeight = false;
    if(w.length() > 1){
        isWeight = true;
    }

    NumericMatrix XtX(K, K);

    if(sparse_check(X) == false){
        // NOT SPARSE

        if(isWeight){
            // I don't use the sqrt, because I use the function when weights are negative too (ll_d2 used as 'weight')
            NumericMatrix wX(Rcpp::clone(X));
            for(int k=0 ; k<K ; ++k){
                for(int i=0 ; i<N ; ++i){
                    wX(i, k) *= w[i];
                }
            }

            // XtX
            mp_XtX(XtX, X, wX, nthreads);

        } else {
            // Identique, mais pas besoin de faire une copie de X ni de y qui peuvent etre couteuses

            // XtX
            mp_XtX(XtX, X, X, nthreads);
        }

        return XtX;
    }

    //
    // SPARSE case
    //

    std::vector<int> n_j(K, 0);
    std::vector<int> start_j(K + 1, 0);
    std::vector<int> all_i;
    std::vector<double> x;

    set_sparse(n_j, start_j, all_i, x, X, w);

    // XtX
    mp_sparse_XtX(XtX, n_j, start_j, all_i, x, X, nthreads);

    return XtX;
}

// [[Rcpp::export]]
NumericMatrix cpp_mat_reconstruct(NumericMatrix X, Rcpp::LogicalVector id_excl){

    int K = id_excl.length();
    int K_small = X.ncol();

    NumericMatrix res(K, K);

    int n_col_excl = 0;
    for(int j=0 ; j<K_small ; ++j){

        while(id_excl[j + n_col_excl]) ++n_col_excl;

        int col = j + n_col_excl;
        int n_row_excl = 0;
        for(int i=0 ; i<K_small ; ++i){
            while(id_excl[i + n_row_excl]) ++n_row_excl;
            res(i+n_row_excl, col) = X(i, j);
        }
    }

    return res;
}


// mp: mat prod
void mp_XZtXZ(NumericMatrix &XZtXZ, const NumericMatrix &XtX, const NumericMatrix &X, const NumericMatrix &Z, const NumericMatrix &wZ, int nthreads){

    int N = Z.nrow();
    int K2 = Z.ncol();

    bool isX = X.nrow() > 1;
    int K1 = isX ? X.ncol() : 0;

    // First: we copy XtX
    for(int k=0 ; k<K1 ; ++k){
        for(int l=0 ; l<K1 ; ++l){
            XZtXZ(k, l) = XtX(k, l);
        }
    }

    //
    // The K1 x K2 band
    //

    // l: index of Z
    // k: index of X

    int nValues = K1 * K2;
    std::vector<int> all_l, all_k;
    for(int l=0 ; l<K2 ; ++l){
        for(int k=0 ; k<K1 ; ++k){
            all_l.push_back(l);
            all_k.push_back(k);
        }
    }

    #pragma omp parallel for num_threads(nthreads)
    for(int index=0 ; index<nValues ; ++index){
        int l = all_l[index];
        int k = all_k[index];

        double val = 0;
        for(int i=0 ; i<N ; ++i){
            val += X(i, k) * wZ(i, l);
        }

        XZtXZ(k, l + K1) = val;
        XZtXZ(l + K1, k) = val;
    }

    //
    // The K2 * K2 mat
    //

    // k, l: indexes of Z
    nValues = K2 * (K2 + 1) / 2;
    all_l.clear();
    all_k.clear();
    for(int l=0 ; l<K2 ; ++l){
        for(int k=l ; k<K2 ; ++k){
            all_l.push_back(l);
            all_k.push_back(k);
        }
    }

    #pragma omp parallel for num_threads(nthreads)
    for(int index=0 ; index<nValues ; ++index){
        int l = all_l[index];
        int k = all_k[index];

        double val = 0;
        for(int i=0 ; i<N ; ++i){
            val += Z(i, k) * wZ(i, l);
        }

        XZtXZ(k + K1, l + K1) = val;
        XZtXZ(l + K1, k + K1) = val;
    }

}

// mp: mat prod
void mp_XZtu(NumericVector &XZtu, const NumericMatrix &X, const NumericMatrix &Z, const double *u, int nthreads){

    int N = Z.nrow();
    int K2 = Z.ncol();

    bool isX = X.nrow() > 1;
    int K1 = isX ? X.ncol() : 0;

    #pragma omp parallel for num_threads(nthreads)
    for(int k=0 ; k<(K1 + K2) ; ++k){
        double val = 0;
        for(int i=0 ; i<N ; ++i){
            if(k < K1){
                val += X(i, k) * u[i];
            } else {
                val += Z(i, k - K1) * u[i];
            }
        }

        XZtu[k] = val;
    }

}

void mp_sparse_XZtXZ(NumericMatrix &XZtXZ, const NumericMatrix &XtX, const std::vector<int> &n_j, const std::vector<int> &start_j, const std::vector<int> &all_i, const std::vector<double> &x, const NumericMatrix &X, const NumericMatrix &Z, const NumericMatrix &wZ, int nthreads){

    int N  = Z.nrow();
    int K2 = Z.ncol();

    bool isX = X.nrow() > 1;
    int K1 = isX ? X.ncol() : 0;

    // First: we copy XtX
    for(int k=0 ; k<K1 ; ++k){
        for(int l=0 ; l<K1 ; ++l){
            XZtXZ(k, l) = XtX(k, l);
        }
    }

    // the K1 x K2 band
    for(int l=0 ; l<K2 ; ++l){
        #pragma omp parallel for num_threads(nthreads) schedule(static, 1)
        for(int j=0 ; j<K1 ; ++j){

            int start = start_j[j];
            int end = start_j[j + 1];

            double value = 0;
            for(int index=start ; index<end ; ++index){
                value += Z(all_i[index], l) * x[index];
            }

            XZtXZ(j, K1 + l) = value;
            XZtXZ(K1 + l, j) = value;
        }
    }

    //
    // The K2 * K2 mat
    //

    // k, l: indexes of Z
    int nValues = K2 * (K2 + 1) / 2;
    std::vector<int> all_l, all_k;
    for(int l=0 ; l<K2 ; ++l){
        for(int k=l ; k<K2 ; ++k){
            all_l.push_back(l);
            all_k.push_back(k);
        }
    }

    #pragma omp parallel for num_threads(nthreads)
    for(int index=0 ; index<nValues ; ++index){
        int l = all_l[index];
        int k = all_k[index];

        double val = 0;
        for(int i=0 ; i<N ; ++i){
            val += Z(i, k) * wZ(i, l);
        }

        XZtXZ(k + K1, l + K1) = val;
        XZtXZ(l + K1, k + K1) = val;
    }
}

void mp_sparse_XZtu(NumericVector &XZtu, const std::vector<int> &start_j, const std::vector<int> &all_i, const std::vector<double> &x, const double *u, const NumericMatrix &X, const NumericMatrix &wZ, int nthreads){

    int N = wZ.nrow();
    int K2 = wZ.ncol();

    bool isX = X.nrow() > 1;
    int K1 = isX ? X.ncol() : 0;

    #pragma omp parallel for num_threads(nthreads)
    for(int k=0 ; k<(K1 + K2) ; ++k){

        double value = 0;
        if(k < K1){
            int start = start_j[k];
            int end = start_j[k + 1];

            for(int index=start ; index<end ; ++index){
                value += u[all_i[index]] * x[index];
            }
        } else {
            for(int i=0 ; i<N ; ++i){
                value += u[i] * wZ(i, k - K1);
            }
        }

        XZtu[k] = value;
    }

}


// [[Rcpp::export]]
List cpp_iv_products(NumericMatrix X, SEXP y, NumericMatrix Z, SEXP u, NumericVector w, int nthreads){
    // We compute the following:
    // - X'X
    // - X'y
    // - (XZ)'(XZ)
    // - (XZ)'u
    //
    // Z: IV mat, u: endo regs

    // Note that X can be equal to 0 (ie no X)

    int N = Z.nrow();
    int K2 = Z.ncol();

    bool isX = X.nrow() > 1;
    int K1 = isX ? X.ncol() : 0;

    bool isWeight = w.length() > 1;

    bool is_y_list = TYPEOF(y) == VECSXP;

    NumericMatrix XtX(K1, K1);
    NumericMatrix XZtXZ(K1 + K2, K1 + K2);

    NumericMatrix wZ;
    if(isWeight){
        wZ = Rcpp::clone(Z);
        for(int k=0 ; k<K2 ; ++k){
            for(int i=0 ; i<N ; ++i){
                wZ(i, k) *= w[i];
            }
        }
    } else {
        // shallow copy
        wZ = Z;
    }

    if(sparse_check(X) == false){
        // NOT SPARSE

        List res;
        NumericMatrix wX;
        if(isWeight){
            wX = Rcpp::clone(X);
            for(int k=0 ; k<K1 ; ++k){
                for(int i=0 ; i<N ; ++i){
                    wX(i, k) *= w[i];
                }
            }
        } else {
            // shallow copy
            wX = X;
        }

        // XtX
        mp_XtX(XtX, X, wX, nthreads);
        res["XtX"] = XtX;

        // (XZ)'(XZ)
        mp_XZtXZ(XZtXZ, XtX, X, Z, wZ, nthreads);
        res["XZtXZ"] = XZtXZ;

        // Xty
        if(!isX){
            NumericVector Xty(1);
            res["Xty"] = Xty;

        } else if(is_y_list){
            int n_vars_y = Rf_length(y);
            List Xty(n_vars_y);

            for(int v=0 ; v<n_vars_y ; ++v){
                NumericVector Xty_tmp(K1);
                mp_Xty(Xty_tmp, wX, REAL(VECTOR_ELT(y, v)), nthreads);
                Xty[v] = Xty_tmp;
            }

            res["Xty"] = Xty;

        } else {
            NumericVector Xty(K1);
            mp_Xty(Xty, wX, REAL(y), nthreads);
            res["Xty"] = Xty;
        }

        // (XZ)'u
        // u is always a list
        int n_vars_u = Rf_length(u);
        List XZtu(n_vars_u);

        for(int v=0 ; v<n_vars_u ; ++v){
            NumericVector XZtu_tmp(K1 + K2);
            mp_XZtu(XZtu_tmp, wX, wZ, REAL(VECTOR_ELT(u, v)), nthreads);
            XZtu[v] = XZtu_tmp;
        }

        res["XZtu"] = XZtu;

        return res;
    }

    //
    // SPARSE case
    //

    std::vector<int> n_j(K1 + !isX, 0);
    std::vector<int> start_j(K1 + !isX + 1, 0);
    std::vector<int> all_i;
    std::vector<double> x;

    set_sparse(n_j, start_j, all_i, x, X, w);

    List res;

    // XtX
    mp_sparse_XtX(XtX, n_j, start_j, all_i, x, X, nthreads);
    res["XtX"] = XtX;

    // (xZ)'(XZ)
    mp_sparse_XZtXZ(XZtXZ, XtX, n_j, start_j, all_i, x, X, Z, wZ, nthreads);
    res["XZtXZ"] = XZtXZ;

    // Xty
    if(!isX){
        NumericVector Xty(1);
        res["Xty"] = Xty;

    } else if(is_y_list){
        int n_vars_y = Rf_length(y);
        List Xty(n_vars_y);

        for(int v=0 ; v<n_vars_y ; ++v){
            NumericVector Xty_tmp(K1);
            mp_sparse_Xty(Xty_tmp, start_j, all_i, x, REAL(VECTOR_ELT(y, v)), nthreads);
            Xty[v] = Xty_tmp;
        }

        res["Xty"] = Xty;

    } else {
        NumericVector Xty(K1);
        mp_sparse_Xty(Xty, start_j, all_i, x, REAL(y), nthreads);
        res["Xty"] = Xty;
    }

    // (XZ)'u
    int n_vars_u = Rf_length(u);
    List XZtu(n_vars_u);

    for(int v=0 ; v<n_vars_u ; ++v){
        NumericVector XZtu_tmp(K1 + K2);
        mp_sparse_XZtu(XZtu_tmp, start_j, all_i, x, REAL(VECTOR_ELT(u, v)), X, wZ, nthreads);
        XZtu[v] = XZtu_tmp;
    }

    res["XZtu"] = XZtu;

    return res;
}


// [[Rcpp::export]]
List cpp_iv_product_completion(NumericMatrix XtX, NumericVector Xty, NumericMatrix X, NumericVector y, NumericMatrix U, NumericVector w, int nthreads){
    // We compute the following
    // - (XU)'(XU)
    // - (XU)'y

    int N = U.nrow();
    int K2 = U.ncol();

    bool isX = X.nrow() > 1;
    int K1 = isX ? X.ncol() : 0;

    bool isWeight = w.length() > 1;

    NumericMatrix XUtXU(K1 + K2, K1 + K2);
    NumericVector XUty(K1 + K2);

    NumericMatrix wU;
    if(isWeight){
        wU = Rcpp::clone(U);
        for(int k=0 ; k<K2 ; ++k){
            for(int i=0 ; i<N ; ++i){
                wU(i, k) *= w[i];
            }
        }
    } else {
        // shallow copy
        wU = U;
    }

    List res;

    // (XU)'y
    for(int k=0 ; k<K1 ; ++k){
        XUty[k] = Xty[k];
    }

    #pragma omp parallel for num_threads(nthreads)
    for(int k=0 ; k<K2 ; ++k){
        double val = 0;
        for(int i=0 ; i<N ; ++i){
            val += y[i] * wU(i, k);
        }

        XUty[k + K1] = val;
    }

    res["XUty"] = XUty;

    // (XU)'(XU)

    if(sparse_check(X) == false){
        mp_XZtXZ(XUtXU, XtX, X, U, wU, nthreads);
        res["XUtXU"] = XUtXU;

    } else {
        std::vector<int> n_j(K1 + !isX, 0);
        std::vector<int> start_j(K1 + !isX + 1, 0);
        std::vector<int> all_i;
        std::vector<double> x;

        set_sparse(n_j, start_j, all_i, x, X, w);

        mp_sparse_XZtXZ(XUtXU, XtX, n_j, start_j, all_i, x, X, U, wU, nthreads);
        res["XUtXU"] = XUtXU;
    }


    return res;
}








