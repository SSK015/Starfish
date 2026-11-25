# Wordcount

Build the two Starfish jobs and their native reference from the repository root.

```bash
JOBS=2 BUILD_CPUS=2 BUILD_MEMORY_GIB=8 bash scripts/build.sh --targets wordcount_far,wordcount_recipes,wordcount_native
```

[Ordinary Wordcount](wordcount_far.cpp) and [Wordcount with recipes](wordcount_recipes.cpp)
use the same [Map, Merge and output implementation](wordcount_job.hpp). Both
publish partition-local tables and then merge them into the global table. The ordinary job uses count-only native rows. The recipe job records
each key's first-insertion ordinal and annotates the partition before publication.
The [native reference](wordcount_native.cpp) runs entirely in local memory.

## Run the pair

[recipe.config](recipe.config) describes six active endpoints and one standby
for 4+2 erasure coding. Set its addresses, ports and device for your fabric,
start the memory services, and point `WORDCOUNT_INPUT` at an immutable input file.

```bash
: "${WORDCOUNT_INPUT:?set to an existing immutable input file}"
bin=build/benchmark/mapreduce
"$bin/wordcount_far"   apps/wordcount/recipe.config "$WORDCOUNT_INPUT" 4 0 20
"$bin/wordcount_recipes" apps/wordcount/recipe.config "$WORDCOUNT_INPUT" 4 0 20
"$bin/wordcount_native" "$WORDCOUNT_INPUT" 4 0 20
```

The trailing arguments select application threads, maximum input bytes (0 reads
the entire file), and the number of ranked words to print. Compare the word
counts and checksum across all three runs. To inspect the application changes,
run `diff -U0 apps/wordcount/wordcount_far.cpp apps/wordcount/wordcount_recipes.cpp`.

## How recovery works

The runtime creates the recovery registry on first binding. The initialized
application worker pool sets the upper bound on parallel partition preparation.
The recipe marks a local output map with the immutable input slice and the same
counting function used for preaggregation. On publication the container binds
each entry before exposing it. On loss it replays the slice and uses the
saved ordinal to recover a key and its count. Replay must introduce distinct
keys in the original first-insertion order. Writing a recovered count
invalidates that entry's recipe.

The default build includes all three Wordcount executables. Set
`-DSTARFISH_WORDCOUNT_RECOMPUTE=OFF` to build the native and ordinary
Wordcount programs without the recipe target. Running the recipe version
requires exclusive caching and EC-batch protection.

## Small recovery regression

The `wordcount_recovery` target exercises a 65,536-key deterministic input on
seven endpoints. It forces a recipe read after an endpoint loss, changes the
restored value, drives cache pressure using a separate EC-protected map, and
checks the new value after remote re-eviction. It also verifies that the old
recipe callback is not run a second time.

Build the opt-in regression and generate its read-only input.

```bash
JOBS=2 BUILD_CPUS=2 BUILD_MEMORY_GIB=8 bash scripts/build.sh --targets wordcount_recovery
python3 apps/wordcount/testdata/generate_small_oracle.py /tmp/wc-oracle
chmod 0444 /tmp/wc-oracle/input.txt
```

Use the seven-endpoint 4+2 EC-batch configuration with one connected standby.
Start all seven memory services, then launch the regression on the compute host.

```bash
set -euo pipefail
gate=$(mktemp -d)
printf "gate=%s\n" "$gate"
FARLIB_WC_RECIPE_READY_PATH="$gate/ready" FARLIB_WC_RECIPE_GO_PATH="$gate/go" build/benchmark/mapreduce/wordcount_recovery apps/wordcount/recipe.config /tmp/wc-oracle/input.txt
```

When `wordcount_recipe_fixture_ready` appears, stop one active endpoint process
from index 0 through 5 and keep endpoint 6 online. In a second compute-host
terminal, enter the gate path printed by the first terminal and create its
release marker.

```bash
read -r -p "Gate directory: " gate
touch "$gate/go"
```

A successful run prints `wordcount_recipe_fixture_result status=pass`.
Restart the stopped memory service after the fixture exits. Remove the two
marker files and temporary directory after reviewing the result.

```bash
rm -f "$gate/ready" "$gate/go"
rmdir "$gate"
```
