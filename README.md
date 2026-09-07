# tetr_bot

## Build

Build the calibration tool and the bot with MinGW-w64:

```powershell
g++ -std=c++17 -O2 calibrate.cpp -o calibrate.exe -lgdi32
g++ -std=c++17 -O2 color.cpp -o color.exe -lgdi32
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

To run the bot while saving and printing every captured state, use:

```powershell
.\color.exe --debug
```

Debug captures are written next to `color.exe` and are ignored by Git.

## Speed testing

The bot is currently configured to search the current piece plus one queued
piece. All rotation, movement, and hard-drop key-down/key-up events for a move
are submitted as one ordered `SendInput` batch without per-key sleeps. Capture
then polls at 1 ms intervals until the detected `NEXT` queue has actually
advanced, instead of relying on a fixed render delay. A high-resolution Windows
timer prevents short waits from being rounded to roughly 15 ms. The previous
one-second loop delay has been removed.

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
