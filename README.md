# tetr_bot

## Build

Build the calibration tool and the bot with MinGW-w64:

```powershell
g++ -std=c++17 -O2 calibrate.cpp -o calibrate.exe -lgdi32
g++ -std=c++17 -O2 color.cpp -o color.exe -lgdi32
g++ -std=c++17 -O2 solver_eval.cpp -o solver_eval.exe -lgdi32
```

The default VS Code build task uses these compiler and linker settings for the
active C++ file.

## Screen calibration

Run `calibrate.exe` before starting the bot. Keep the game visible and paused,
then follow the console prompts to select:

1. The top-left and bottom-right outer corners of the 10 x 20 board grid.
2. The top-left and bottom-right of the black `NEXT` preview interior, excluding
   its white header and border.

The tool writes `screen_layout.cfg` next to the executable. The bot reads this
file at startup, allocates a pixel buffer covering both selected regions, and
derives every board cell center from the selected board size.

Because occupancy is sampled from a 3 x 3 area at each cell's center, outlined
ghost cells continue to be treated as empty.

## Capture debugging

To inspect one frame without letting the bot play, run:

```powershell
.\color.exe --inspect
```

Press `P` once the game is visible. The program saves the exact captured pixels
as `debug_capture_0.bmp`, prints the detected 20 x 10 grid using `#` and `.`, and
prints the detected `NEXT` queue with the sampled RGB value for every slot.
Queue colors are matched by chromaticity rather than raw brightness, so the
same piece is recognized consistently across the preview's highlights,
shadows, and locked-block shading.

To run the bot while printing every tracked state, use:

```powershell
.\color.exe --debug
```

For normal play, press `P` while the opening five-piece queue is visible, before
the game starts. Its top piece is treated as the human-controlled opening piece.
Start the game and hard-drop that piece at its default position and orientation.
Because the board was empty, the bot constructs the resulting board directly,
takes control with the second opening piece, and does not capture startup board
graphics. Later states are simulated, while a periodic physical board audit can
repair drift. Audits begin after a one-second warm-up and run every 200 moves;
the bot keeps playing during the warm-up.

## Placement evaluation

Candidate boards are scored for aggregate and maximum height, surface
bumpiness, holes, the depth of blocks burying those holes, wells, and row and
column transitions. Height above row 12 receives an additional nonlinear
danger penalty. Line clears use a bounded reward so the solver does not create
holes merely to chase a Tetris. Every intermediate lookahead placement also
receives a large immediate hole penalty, so a hole that the following piece
could theoretically repair is still avoided whenever a clean move exists.

## Automated solver evaluation

`solver_eval.exe` runs the production solver without capturing the screen or
sending keyboard input. It generates deterministic seven-bag queues, asks the
same `getBestPosIterative` function for each move, applies the selected piece,
and measures survival, line clears, holes, stack height, and search latency.

For a comparison run:

```powershell
.\solver_eval.exe --games 16 --max-pieces 2000 --lookahead 1 --seed 2 --threads 16 --label baseline
```

The same seed produces the same independent game sequences. Increase
`--max-pieces` when games regularly reach the cap; a capped game has not topped
out and therefore does not establish its true survival length. Results are
appended to `solver_eval_results.csv`. Use `--no-csv` for temporary runs and
`--help` to see all options.

Games run in parallel, process-isolated workers so the production solver's
global board and recursion state cannot race between games. `--threads 0`, the
default, automatically uses up to the machine's logical CPU count or the number
of games, whichever is smaller. Use `--threads 1` for a serial baseline. The
summary reports both per-worker solver throughput and actual parallel
wall-clock throughput, and stores the worker count in the CSV.

The search itself uses a compact 10-bit-per-row board and allocation-free
recursion. For multi-game evaluation, keep `--search-threads 1` because the
games already occupy the CPU cores. To benchmark one depth-3 game using the
same root-level parallelism as the live bot, use:

```powershell
.\solver_eval.exe --games 1 --lookahead 3 --threads 1 --search-threads 0
```

For `--search-threads`, zero selects the available logical CPUs.

## Speed testing

The bot defaults to the lookahead configured in `color.cpp`. It can be changed
without recompiling, for example:

```powershell
.\color.exe --lookahead 2
.\color.exe --lookahead 3
```

Depth 3 automatically evaluates root placements in parallel; shallower depths
avoid thread-launch overhead. All rotation, movement, and hard-drop key-down/key-up events for a move
are submitted in one `SendInput` batch. A physical capture reads the entire
five-piece `NEXT` queue. With lookahead 1, the bot then consumes four pieces
from that local queue before capturing it again; the fifth piece is retained as
an overlap check. Higher lookahead values automatically shorten the batch when
needed. At each refresh, the captured queue must be reliable, different from
the preceding full capture, and begin with every locally known overlap piece.
Empty, low-confidence, and inconsistent readings are retried instead of being
accepted. A five-millisecond initial settle wait gives the final hard drop one
200 Hz display interval to become visible, and a high-resolution Windows timer
prevents short waits from being rounded to roughly 15 ms.

The benchmark summary reports how often the board was resynchronized and how
many resynchronizations corrected drift.

Run without `--debug` when benchmarking. Normal mode reports fractional search
time, full capture-to-capture cycle time, and instantaneous pieces per second
every 25 moves so Windows console rendering does not throttle the bot. Debug
mode retains per-move output.
The VS Code task and commands above use `-O2` optimization. Press `P` to stop
and print the averages.

The timing report separates:

- Board preparation and solver search.
- Placement input and the post-drop render wait.
- Amortized queue capture, simulated board update, and queue decoding.
- Total placing, looking, and full-cycle time.

Stopping normally or encountering an invalid simulated placement prints sample
count, average, median, 95th percentile, minimum, and maximum for every stage.
Each run also appends one summary row to `benchmark_results.csv` next to the
executable so results can be compared across versions.
