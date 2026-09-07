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
file at startup, captures the smallest rectangle containing both selected
regions, and derives every board cell center from the selected board size.

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

To run the bot while saving and printing every captured state, use:

```powershell
.\color.exe --debug
```

Debug captures are written next to `color.exe` and are ignored by Git.

At game startup, the board reader detects the wide yellow `GO!` overlay and
ignores its yellow samples. You can press the first hard drop while `GO!` is
still visible instead of waiting for it to disappear. Detection permanently
turns off as soon as the overlay is absent, preventing yellow blocks later in
the game from being mistaken for another startup overlay. If no `GO!` appears,
the detector disables itself on the first capture and normal play is unchanged.

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
.\solver_eval.exe --games 10 --max-pieces 2000 --lookahead 1 --seed 2 --label baseline
```

The same seed produces the same independent game sequences. Increase
`--max-pieces` when games regularly reach the cap; a capped game has not topped
out and therefore does not establish its true survival length. Results are
appended to `solver_eval_results.csv`. Use `--no-csv` for temporary runs and
`--help` to see all options.

## Speed testing

The bot is currently configured to search the current piece plus one queued
piece. All rotation, movement, and hard-drop key-down/key-up events for a move
are submitted in one `SendInput` batch with no artificial hold or settling
delay. Capture polls at 1 ms intervals until the detected `NEXT` queue has
actually advanced, then accepts that first updated frame. Each slot is sampled
through its central vertical band so adjacent previews crossing slot boundaries
during animation are ignored. A high-resolution Windows timer prevents short
waits from being rounded to roughly 15 ms. The previous one-second loop delay
has been removed.

After each move, the captured board is compared with the board predicted by the
solver (excluding the active piece's spawn rows). A mismatch is printed
immediately. Every mismatch is appended to `placement_mismatches.csv`, including
the planned piece, position, rotation, queue, and expected and captured boards.
The first five mismatch images are also saved with those choices in the
filename (for example, `placement_mismatch_move_42_O_x2_r0_diff4.bmp`).
The final benchmark summary reports mismatch counts and percentages separately
for every tetromino type.

Run without `--debug` when benchmarking. Each move reports fractional search
time, full capture-to-capture cycle time, and instantaneous pieces per second.
The VS Code task and commands above use `-O2` optimization. Press `P` to stop
and print the averages.

The timing report separates:

- Board preparation and solver search.
- Placement input and the post-drop render wait.
- Screen capture, grid decoding, and queue decoding.
- Total placing, looking, and full-cycle time.

Stopping normally or encountering an invalid simulated placement prints sample
count, average, median, 95th percentile, minimum, and maximum for every stage.
Each run also appends one summary row to `benchmark_results.csv` next to the
executable so results can be compared across versions.
