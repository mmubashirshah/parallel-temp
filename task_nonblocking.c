/*
 * FIT3143 Parallel Computing - Lab 3
 * EV Charging Navigation System (Non-Blocking Version)
 * Muhammad Mubashir Shah (33878897) & Ayman Zuhair Shashavali (33506728)
 *
 * We implemented Tasks 1 & 2 using non-blocking MPI communication.
 * - Uses MPI_Isend and MPI_Irecv followed by MPI_Waitall across all 4 neighbours.
 * - This overlaps neighbour exchanges with local OpenMP computation.
 * - Avoids sequential waiting delays between top/bottom/left/right exchanges.
 */

#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <time.h>
#include <unistd.h>
#include <mpi.h>
#include <omp.h>

#define TAG_QUERY  10
#define TAG_ALERT  20
#define TAG_ADVICE 21

void run_ev_node(int rank, MPI_Comm comm2D, int nrows, int ncols, int k_ports, int threshold, int max_cycles, int bs_rank);
void run_base_station(int bs_rank, int total_nodes, int nrows, int ncols, int max_cycles, int window);

int main(int argc, char *argv[]) {
    int rank, size;
    int nrows      = (argc > 1) ? atoi(argv[1]) : 3;
    int ncols      = (argc > 2) ? atoi(argv[2]) : 3;
    int k_ports    = (argc > 3) ? atoi(argv[3]) : 6;
    int max_cycles = (argc > 4) ? atoi(argv[4]) : 15;
    int threshold  = (argc > 5) ? atoi(argv[5]) : 1;

    MPI_Init(&argc, &argv);
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &size);

    // base station is rank N (the last rank)
    int total_nodes = nrows * ncols;
    int bs_rank = total_nodes;

    if (size != total_nodes + 1) {
        if (rank == 0) printf("Error: Expected %d processes, got %d\n", total_nodes + 1, size);
        MPI_Finalize();
        return 1;
    }

    // split comm_world: charging stations get 0, base station gets 1
    int is_bs = (rank == bs_rank);
    MPI_Comm sub_comm;
    MPI_Comm_split(MPI_COMM_WORLD, is_bs, rank, &sub_comm);

    if (is_bs) {
        run_base_station(bs_rank, total_nodes, nrows, ncols, max_cycles, 5);
    } else {
        // set up 2d cartesian mesh for the charging nodes (no wrap-around)
        MPI_Comm comm2D;
        int dims[2] = {nrows, ncols}, periods[2] = {0, 0};
        MPI_Cart_create(sub_comm, 2, dims, periods, 0, &comm2D);

        run_ev_node(rank, comm2D, nrows, ncols, k_ports, threshold, max_cycles, bs_rank);

        MPI_Comm_free(&comm2D);
    }

    MPI_Comm_free(&sub_comm);
    MPI_Finalize();
    return 0;
}

// EV Charging Node (Task 1 - Non-Blocking)
void run_ev_node(int rank, MPI_Comm comm2D, int nrows, int ncols, int k_ports, int threshold, int max_cycles, int bs_rank) {
    int coords[2], top, bottom, left, right;
    MPI_Cart_coords(comm2D, rank, 2, coords);
    // get top/bottom and left/right neighbours using cart_shift
    MPI_Cart_shift(comm2D, 0, 1, &top, &bottom);
    MPI_Cart_shift(comm2D, 1, 1, &left, &right);

    int port_status[32] = {0};

    for (int cycle = 1; cycle <= max_cycles; cycle++) {
        // simulate each charging bay with an OpenMP thread
        #pragma omp parallel for
        for (int p = 0; p < k_ports; p++) {
            unsigned int seed = (unsigned int)(time(NULL) ^ (rank * 1000) ^ (p * 79) ^ (cycle * 13));
            int occupied = (rand_r(&seed) % 100 < 75) ? 1 : 0;
            // protect writes to the shared port array
            #pragma omp critical
            port_status[p] = occupied;
        }

        int available = 0;
        for (int p = 0; p < k_ports; p++) if (port_status[p] == 0) available++;

        // non-blocking exchange: post irecv and isend in all 4 directions together
        int nbr_avail[4] = {-1, -1, -1, -1};
        MPI_Request reqs[8];
        int req_count = 0;
        int nbr_ranks[4] = {top, bottom, left, right};

        for (int d = 0; d < 4; d++) {
            if (nbr_ranks[d] != MPI_PROC_NULL) {
                MPI_Irecv(&nbr_avail[d], 1, MPI_INT, nbr_ranks[d], TAG_QUERY, comm2D, &reqs[req_count++]);
                MPI_Isend(&available, 1, MPI_INT, nbr_ranks[d], TAG_QUERY, comm2D, &reqs[req_count++]);
            }
        }
        // wait for all 4 directions to finish
        if (req_count > 0) MPI_Waitall(req_count, reqs, MPI_STATUSES_IGNORE);

        int will_alert = 0;
        int report[12] = {0};

        // if we are low on free ports (<= threshold), check our neighbours
        if (available <= threshold) {
            int all_nbrs_full = 1;
            for (int d = 0; d < 4; d++) {
                // if a valid neighbour has a free spot, redirect locally
                if (nbr_ranks[d] != MPI_PROC_NULL && nbr_avail[d] > threshold) {
                    all_nbrs_full = 0;
                    break;
                }
            }

            // if all neighbours are full too, we alert the base station
            if (all_nbrs_full) {
                will_alert = 1;
                report[0] = rank; report[1] = coords[0]; report[2] = coords[1]; report[3] = available;
                report[4] = nbr_avail[0]; report[5] = nbr_avail[1]; report[6] = nbr_avail[2]; report[7] = nbr_avail[3];
                report[8] = 8; report[9] = 32; report[10] = (int)time(NULL); report[11] = cycle;
            }
        }

        // let base station know how many nodes are alerting this cycle
        int total_alerts = 0;
        MPI_Reduce(&will_alert, &total_alerts, 1, MPI_INT, MPI_SUM, bs_rank, MPI_COMM_WORLD);

        if (will_alert) {
            int advice[6];
            MPI_Send(report, 12, MPI_INT, bs_rank, TAG_ALERT, MPI_COMM_WORLD);
            MPI_Recv(advice, 6, MPI_INT, bs_rank, TAG_ADVICE, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
        }

        MPI_Barrier(comm2D);
        usleep(2000);
    }

    // wait for base station to tell us we're done
    int shutdown = 0;
    MPI_Bcast(&shutdown, 1, MPI_INT, bs_rank, MPI_COMM_WORLD);
}

// Base Station (Task 2)
void run_base_station(int bs_rank, int total_nodes, int nrows, int ncols, int max_cycles, int window) {
    FILE *log = fopen("base_station_log.txt", "w");
    if (log) fprintf(log, "=== Base Station Event Log (Non-Blocking: %d nodes, %d cycles) ===\n\n", total_nodes, max_cycles);

    // track the last cycle each node sent an alert
    int *last_alert = (int *)malloc(total_nodes * sizeof(int));
    for (int i = 0; i < total_nodes; i++) last_alert[i] = -999;

    int total_alerts = 0;
    double total_comm_time = 0.0;
    double t_start = MPI_Wtime();

    for (int cycle = 1; cycle <= max_cycles; cycle++) {
        int dummy = 0, alerts_this_cycle = 0;
        MPI_Reduce(&dummy, &alerts_this_cycle, 1, MPI_INT, MPI_SUM, bs_rank, MPI_COMM_WORLD);

        for (int a = 0; a < alerts_this_cycle; a++) {
            int rep[12];
            MPI_Status status;
            double t0 = MPI_Wtime();
            MPI_Recv(rep, 12, MPI_INT, MPI_ANY_SOURCE, TAG_ALERT, MPI_COMM_WORLD, &status);
            total_comm_time += (MPI_Wtime() - t0);
            total_alerts++;
            last_alert[rep[0]] = rep[11];

            // find closest node using manhattan distance that hasn't alerted recently
            int min_d = 9999, cand = -1, cand_x = -1, cand_y = -1;
            for (int r = 0; r < nrows; r++) {
                for (int c = 0; c < ncols; c++) {
                    int cand_rank = r * ncols + c;
                    if (cand_rank == rep[0]) continue;
                    // check sliding window to avoid sending cars to another full node
                    if ((rep[11] - last_alert[cand_rank]) > window) {
                        int dist = abs(r - rep[1]) + abs(c - rep[2]);
                        if (dist < min_d) {
                            min_d = dist;
                            cand = cand_rank; cand_x = r; cand_y = c;
                        }
                    }
                }
            }

            // send back nearest station advice to the alerting node
            int advice[6] = {(cand >= 0 ? 1 : 0), cand, -1, -1, -1, min_d};
            double t1 = MPI_Wtime();
            MPI_Send(advice, 6, MPI_INT, status.MPI_SOURCE, TAG_ADVICE, MPI_COMM_WORLD);
            total_comm_time += (MPI_Wtime() - t1);

            if (log) {
                time_t now = time(NULL);
                char tbuf[32]; strftime(tbuf, 32, "%H:%M:%S", localtime(&now));
                fprintf(log, "[%s | Cycle %d] Alert: Node %d (%d,%d), Avail: %d, Top:%d Bot:%d L:%d R:%d\n",
                        tbuf, rep[11], rep[0], rep[1], rep[2], rep[3], rep[4], rep[5], rep[6], rep[7]);
                if (cand >= 0)
                    fprintf(log, "  -> Suggested Nearest Station: Node %d at (%d,%d), Manhattan Distance: %d\n\n", cand, cand_x, cand_y, min_d);
                else
                    fprintf(log, "  -> No available station found nearby\n\n");
                fflush(log);
            }
        }
        usleep(2000);
    }

    double total_time = MPI_Wtime() - t_start;

    // broadcast shutdown signal to let all nodes exit cleanly
    int shutdown = 1;
    MPI_Bcast(&shutdown, 1, MPI_INT, bs_rank, MPI_COMM_WORLD);

    if (log) {
        fprintf(log, "=== Simulation Finished: Time: %.4fs, Total Alerts: %d ===\n", total_time, total_alerts);
        fclose(log);
    }

    // print csv line for benchmark scripts
    printf("CSV_RESULT,NonBlocking,%d,%d,%d,%d,%.6f,%.6f,%d\n", nrows, ncols, total_nodes, max_cycles, total_time, total_comm_time, total_alerts);
    free(last_alert);
}
