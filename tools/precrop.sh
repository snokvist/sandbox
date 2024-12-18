#!/bin/sh

# Function to display error messages and exit
error_exit() {
    echo "Error: $1"
    exit 1
}

# Check if a precrop string is provided as an argument
if [ -n "$1" ]; then
    result=$(echo "$1" | tr -d '[:space:]')
else
    # Fetch the precrop string using yaml-cli
    result=$(yaml-cli -g .fpv.precrop | tr -d '[:space:]')
fi

# Check if the result is empty
if [ -z "$result" ]; then
    error_exit "No data returned from yaml-cli or argument is empty."
fi

# Split the result using 'x' as the separator and capture into variables
set -- $(echo "$result" | sed 's/x/ /g')

# Check if exactly 6 values were extracted
if [ $# -ne 6 ]; then
    error_exit "Invalid data format. Expected 6 values separated by 'x'."
fi

# Check if the target file exists and is writable
TARGET_FILE="/proc/mi_modules/mi_vpe/mi_vpe0"

if [ ! -e "$TARGET_FILE" ]; then
    error_exit "Target file $TARGET_FILE does not exist."
fi

if [ ! -w "$TARGET_FILE" ]; then
    error_exit "Target file $TARGET_FILE is not writable."
fi

# Write the command to the target file
echo "setprecrop $1 $2 $3 $4 $5 $6" > "$TARGET_FILE" 2>/dev/null

# Check if the write was successful
if [ $? -ne 0 ]; then
    error_exit "Failed to write to $TARGET_FILE."
fi
