#include "wordcount_job.hpp"

using namespace starfish::wordcount;

int main(int argc, char **argv) {
    auto map_partition =
        [](starfish::DereferenceScope &scope, auto &local_maps,
           size_t task_id, Chunk chunk, auto &rows, auto &count_partition) {
            scope.mark_recomputable(local_maps[task_id], chunk, count_partition);
            return count_partition(rows, chunk);
        };
    return run_wordcount_job<true>(argc, argv, map_partition);
}
