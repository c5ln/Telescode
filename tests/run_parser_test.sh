#!/bin/bash
set -e

# Build
cd /home/cheonsh/Telescode
cmake -B build -S . -DCMAKE_BUILD_TYPE=Debug > /dev/null
cmake --build build --target TelescodeParser -j$(nproc)

# Run parser
mkdir -p tests/output
./build/TelescodeParser tests/fixtures/ tests/output/

# Check CSVs exist
for f in file.csv class.csv base_class.csv function.csv param.csv link.csv; do
    if [ ! -f "tests/output/$f" ]; then
        echo "FAIL: tests/output/$f not found"
        exit 1
    fi
done

echo "=== file.csv ==="
cat tests/output/file.csv

echo "=== class.csv ==="
cat tests/output/class.csv

echo "=== base_class.csv ==="
cat tests/output/base_class.csv

echo "=== function.csv ==="
cat tests/output/function.csv

echo "=== param.csv ==="
cat tests/output/param.csv

echo "=== link.csv ==="
cat tests/output/link.csv

# Validate expected rows
if ! grep -q "Dog" tests/output/class.csv; then
    echo "FAIL: Dog class not found in class.csv"
    exit 1
fi

if ! grep -q "Animal" tests/output/base_class.csv; then
    echo "FAIL: Animal base class not found in base_class.csv"
    exit 1
fi

if ! grep -q "speak" tests/output/function.csv; then
    echo "FAIL: speak method not found in function.csv"
    exit 1
fi

if ! grep -q "greet" tests/output/function.csv; then
    echo "FAIL: greet function not found in function.csv"
    exit 1
fi

if ! grep -q "IMPORTS" tests/output/link.csv; then
    echo "FAIL: IMPORTS link not found in link.csv"
    exit 1
fi

echo "ALL CHECKS PASSED"
