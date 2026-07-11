#!/bin/bash
echo "- Skipping fenrir patches (already applied)"

# Limit parallel build jobs to avoid OOM on 16c/30GB system
export NINJA_NUM_JOBS=12
export NINJA_HIGHMEM_NUM_JOBS=4
