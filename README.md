# CSOPESY_MO1


## Group Developers

- Austria, Ma. Alexandria
- De Leon, Sofia Ysabela
- Guererro, Laura Mae
- Patricio, Anne Beatriz

## Entry Class / Main Function

- **Entry file:** `main.cpp`
- **Main function:** `main()` in `main.cpp`

## Program Requirements

- C++ compiler with C++17 support (MinGW / GCC recommended)
- Windows OS (supports Windows Console API and ANSI escape sequences)

## How to Compile

Using g++/MinGW on Windows:

```bash
g++ -std=c++17 -O2 -o main main.cpp -lpthread
```

This creates the executable:

```text
main.exe
```

## How to Run

After compiling, run:

```bash
main.exe
```

Or from the terminal:

```bash
./main.exe
```

## Program Commands

### `initialize`

Initializes the system and scheduler configuration from `config.txt`. This must be run before any scheduler or screen commands can be used.

### `screen -s <name>`

Creates and attaches to a new process screen.

### `screen -r <name>`

Reattaches to an active process screen.

### `screen -ls`

Displays CPU utilization, core usage, and a list of running and finished processes.

### `scheduler-start`

Starts the continuous generation of dummy processes for the scheduler.

### `scheduler-stop`

Stops the generation of dummy processes.

### `report-util`

Generates and saves the CPU utilization and process report to `csopesy-log.txt`.

### `clear` / `cls`

Clears the screen and redraws the main menu.

### `exit`

Terminates the CLI emulator.

## Basic Usage Example

1. Compile the program using the compilation command above.
2. Run `main.exe`.
3. Initialize the application first:

```text
initialize
```

4. View process status:

```text
screen -ls
```

5. Generate test processes:

```text
scheduler-start
```

6. Stop test process generation:

```text
scheduler-stop
```

7. Save a report to file:

```text
report-util
```

8. Exit the emulator:

```text
exit
```