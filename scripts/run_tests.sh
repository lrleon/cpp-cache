#!/bin/bash

# Configuration
# We use existing directories if they are standard or just create our own.
# Since the repo has multiple build folders, we'll use build-debug and build-release.
BUILD_DIR_DEBUG="build-debug"
BUILD_DIR_RELEASE="build-release"
CORES=$(nproc 2>/dev/null || echo 4)

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

function log_info() {
    echo -e "${BLUE}[INFO]${NC} $1"
}

function log_success() {
    echo -e "${GREEN}[SUCCESS]${NC} $1"
}

function log_warning() {
    echo -e "${YELLOW}[WARNING]${NC} $1"
}

function log_error() {
    echo -e "${RED}[ERROR]${NC} $1"
}

# Function to build and test
# Returns 0 on success, non-zero on failure.
build_and_test() {
    local build_type=$1
    local build_dir=$2
    local start_time
    start_time=$(date +%s) || { log_error "Failed to get start time"; return 1; }

    log_info "--------------------------------------------------------"
    log_info "Starting $build_type build and tests..."
    log_info "--------------------------------------------------------"

    # Create build directory
    if [ ! -d "$build_dir" ]; then
        mkdir -p "$build_dir"
    fi
    
    pushd "$build_dir" > /dev/null || { log_error "Failed to enter directory $build_dir"; return 1; }

    # Configure
    log_info "Configuring $build_type..."
    # -DBUILD_TESTS=OFF and -DBUILD_EXAMPLES=OFF are passed to Aleph-w 
    # to avoid including its 280+ tests and examples.
    if ! cmake -DCMAKE_BUILD_TYPE="$build_type" -DBUILD_TESTS=OFF -DBUILD_EXAMPLES=OFF ..; then
        log_error "Configuration failed for $build_type"
        popd > /dev/null
        return 1
    fi

    # Build
    log_info "Building $build_type with $CORES cores..."
    if ! cmake --build . -j "$CORES"; then
        log_error "Build failed for $build_type"
        popd > /dev/null
        return 1
    fi

    # Run Tests
    log_info "Running tests for $build_type..."
    # --output-on-failure shows the output of failing tests
    # -L cpp_cache ensures we only run tests belonging to this project
    if ! ctest --output-on-failure -L cpp_cache; then
        log_error "Tests failed for $build_type"
        popd > /dev/null
        return 1
    fi

    local end_time
    end_time=$(date +%s)
    local duration=$((end_time - start_time))
    
    log_success "$build_type completed successfully in $duration seconds."
    popd > /dev/null
    return 0
}

# Ensure we are in the project root (check for CMakeLists.txt)
if [ ! -f "CMakeLists.txt" ]; then
    log_error "Error: CMakeLists.txt not found. Please run this script from the project root."
    exit 1
fi

# Main execution logic

# 1. Run Debug
build_and_test "Debug" "$BUILD_DIR_DEBUG"
DEBUG_RESULT=$?

if [ $DEBUG_RESULT -ne 0 ]; then
    log_error "Pipeline stopped: Debug phase failed."
    exit $DEBUG_RESULT
fi

# 2. Run Release
build_and_test "Release" "$BUILD_DIR_RELEASE"
RELEASE_RESULT=$?

if [ $RELEASE_RESULT -ne 0 ]; then
    log_error "Pipeline stopped: Release phase failed."
    exit $RELEASE_RESULT
fi

echo ""
log_success "========================================================"
log_success "SUMMARY: All tests passed in both Debug and Release modes!"
log_success "========================================================"
