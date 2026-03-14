#!/usr/bin/env python3
"""
Generate VP-Tree for color matching in Flash memory.

This script:
1. Reads color data from a JSON file containing xkcd colors
2. Builds a VP-Tree (Vantage Point Tree) for efficient O(log n) nearest neighbor search
3. Outputs a C++ header file with pre-computed VP-Tree as const data in Flash
4. Uses CIEDE2000 as the distance metric

The VP-Tree is stored as a flattened array for cache-friendly access and zero runtime
memory allocations. Each node stores:
- color_index: Index into the color database
- median_distance: Distance threshold for left/right partitioning
- left_child: Index of left child node (colors closer than median)
- right_child: Index of right child node (colors farther than median)

Usage:
    python generate_vptree.py [--input FILE] [--output FILE]

Arguments:
    --input FILE    Input JSON file (default: xkcd_colors.json)
    --output FILE   Output C++ file (default: ../main/vptree_data.h)
"""

import argparse
import json
import math
import os
import sys
from dataclasses import dataclass
from typing import List, Tuple, Optional
import random


@dataclass
class Color:
    """Represents a color with name, LAB values, and index."""
    index: int
    name: str
    l: float
    a: float
    b: float


def load_colors_from_json(filename: str) -> List[Color]:
    """Load colors from a JSON file."""
    with open(filename, 'r', encoding='utf-8') as f:
        data = json.load(f)
    
    colors = []
    for i, item in enumerate(data.get('colors', data)):
        if 'lab' in item:
            lab = item['lab']
            l, a, b = lab[0], lab[1], lab[2]
        else:
            l, a, b = item['L'], item['a'], item['b']
        
        colors.append(Color(
            index=i,
            name=item['name'],
            l=l,
            a=a,
            b=b
        ))
    
    return colors


def ciede2000(lab1: Tuple[float, float, float], lab2: Tuple[float, float, float]) -> float:
    """
    Calculate CIEDE2000 color difference.
    
    This is a Python implementation of the CIEDE2000 formula for building the VP-Tree.
    The runtime C implementation in color_math.cpp is used for actual matching.
    """
    L1, a1, b1 = lab1
    L2, a2, b2 = lab2
    
    # Weighting factors
    kL = 1.0
    kC = 1.0
    kH = 1.0
    
    # Step 1: Calculate C'i and h'i
    C1 = math.sqrt(a1 * a1 + b1 * b1)
    C2 = math.sqrt(a2 * a2 + b2 * b2)
    C_bar = (C1 + C2) / 2.0
    
    C_bar_7 = C_bar ** 7
    G = 0.5 * (1.0 - math.sqrt(C_bar_7 / (C_bar_7 + 25.0 ** 7)))
    
    a1_prime = a1 * (1.0 + G)
    a2_prime = a2 * (1.0 + G)
    
    C1_prime = math.sqrt(a1_prime * a1_prime + b1 * b1)
    C2_prime = math.sqrt(a2_prime * a2_prime + b2 * b2)
    
    def calc_h_prime(a_prime: float, b: float) -> float:
        if abs(a_prime) < 1e-10 and abs(b) < 1e-10:
            return 0.0
        h = math.degrees(math.atan2(b, a_prime))
        if h < 0:
            h += 360.0
        return h
    
    h1_prime = calc_h_prime(a1_prime, b1)
    h2_prime = calc_h_prime(a2_prime, b2)
    
    # Step 2: Calculate ΔL', ΔC', ΔH'
    delta_L_prime = L2 - L1
    delta_C_prime = C2_prime - C1_prime
    
    if C1_prime * C2_prime < 1e-10:
        delta_h_prime = 0.0
    else:
        dh = h2_prime - h1_prime
        if dh > 180.0:
            dh -= 360.0
        elif dh < -180.0:
            dh += 360.0
        delta_h_prime = dh
    
    delta_H_prime = 2.0 * math.sqrt(C1_prime * C2_prime) * math.sin(math.radians(delta_h_prime / 2.0))
    
    # Step 3: Calculate CIEDE2000 color difference
    L_bar_prime = (L1 + L2) / 2.0
    C_bar_prime = (C1_prime + C2_prime) / 2.0
    
    if C1_prime * C2_prime < 1e-10:
        h_bar_prime = h1_prime + h2_prime
    else:
        h_sum = h1_prime + h2_prime
        if abs(h1_prime - h2_prime) > 180.0:
            if h_sum < 360.0:
                h_sum += 360.0
            else:
                h_sum -= 360.0
        h_bar_prime = h_sum / 2.0
    
    T = (1.0 
         - 0.17 * math.cos(math.radians(h_bar_prime - 30.0))
         + 0.24 * math.cos(math.radians(2.0 * h_bar_prime))
         + 0.32 * math.cos(math.radians(3.0 * h_bar_prime + 6.0))
         - 0.20 * math.cos(math.radians(4.0 * h_bar_prime - 63.0)))
    
    delta_theta = 30.0 * math.exp(-((h_bar_prime - 275.0) / 25.0) ** 2)
    
    C_bar_prime_7 = C_bar_prime ** 7
    RC = 2.0 * math.sqrt(C_bar_prime_7 / (C_bar_prime_7 + 25.0 ** 7))
    
    L_bar_prime_minus_50_sq = (L_bar_prime - 50.0) ** 2
    SL = 1.0 + (0.015 * L_bar_prime_minus_50_sq) / math.sqrt(20.0 + L_bar_prime_minus_50_sq)
    SC = 1.0 + 0.045 * C_bar_prime
    SH = 1.0 + 0.015 * C_bar_prime * T
    
    RT = -math.sin(math.radians(2.0 * delta_theta)) * RC
    
    delta_E = math.sqrt(
        (delta_L_prime / (kL * SL)) ** 2 +
        (delta_C_prime / (kC * SC)) ** 2 +
        (delta_H_prime / (kH * SH)) ** 2 +
        RT * (delta_C_prime / (kC * SC)) * (delta_H_prime / (kH * SH))
    )
    
    return delta_E


@dataclass
class VPTreeNode:
    """Represents a node in the VP-Tree."""
    color_index: int        # Index into color database
    median_distance: float  # Distance threshold for partitioning
    left_child: int         # Index of left child (-1 if none)
    right_child: int        # Index of right child (-1 if none)


def build_vptree(colors: List[Color], indices: List[int], node_list: List[VPTreeNode]) -> int:
    """
    Recursively build the VP-Tree.
    
    Returns the index of the created node in node_list, or -1 if empty.
    """
    if not indices:
        return -1
    
    if len(indices) == 1:
        # Leaf node
        node = VPTreeNode(
            color_index=colors[indices[0]].index,
            median_distance=0.0,
            left_child=-1,
            right_child=-1
        )
        node_list.append(node)
        return len(node_list) - 1
    
    # Choose vantage point (use first element, could also use random selection)
    vp_idx = indices[0]
    vp = colors[vp_idx]
    vp_lab = (vp.l, vp.a, vp.b)
    
    # Calculate distances from vantage point to all other points
    rest_indices = indices[1:]
    distances = []
    for idx in rest_indices:
        c = colors[idx]
        dist = ciede2000(vp_lab, (c.l, c.a, c.b))
        distances.append((idx, dist))
    
    # Sort by distance and find median
    distances.sort(key=lambda x: x[1])
    
    # Split at median
    median_pos = len(distances) // 2
    if median_pos == 0:
        median_distance = distances[0][1] if distances else 0.0
    else:
        median_distance = distances[median_pos - 1][1]
    
    left_indices = [idx for idx, dist in distances if dist <= median_distance]
    right_indices = [idx for idx, dist in distances if dist > median_distance]
    
    # Ensure we make progress (avoid infinite recursion)
    if len(left_indices) == len(distances):
        # All points are at the same distance, split arbitrarily
        left_indices = [idx for idx, _ in distances[:median_pos]]
        right_indices = [idx for idx, _ in distances[median_pos:]]
    
    # Create node first (reserves index)
    node_idx = len(node_list)
    node = VPTreeNode(
        color_index=colors[vp_idx].index,
        median_distance=median_distance,
        left_child=-1,
        right_child=-1
    )
    node_list.append(node)
    
    # Recursively build children
    node_list[node_idx].left_child = build_vptree(colors, left_indices, node_list)
    node_list[node_idx].right_child = build_vptree(colors, right_indices, node_list)
    
    return node_idx


def generate_vptree_header(colors: List[Color], node_list: List[VPTreeNode], output_path: str):
    """Generate the C++ header file with VP-Tree data."""
    
    header_content = '''/**
 * @file vptree_data.h
 * @brief Pre-computed VP-Tree for O(log n) color matching
 * 
 * This file is auto-generated by generate_vptree.py
 * DO NOT EDIT MANUALLY
 * 
 * VP-Tree (Vantage Point Tree) enables efficient nearest neighbor search
 * using CIEDE2000 as the distance metric. The tree is stored as a const
 * array in Flash memory with zero runtime memory allocations.
 * 
 * Tree properties:
 * - Number of colors: {num_colors}
 * - Number of nodes: {num_nodes}
 * - Average search complexity: O(log n)
 * - Memory usage: {memory_kb:.2f} KB in Flash
 */

#ifndef VPTREE_DATA_H
#define VPTREE_DATA_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {{
#endif

/**
 * @brief VP-Tree node structure
 * 
 * Each node stores:
 * - color_index: Index into the color database (xkcd_colors array)
 * - median_distance: Distance threshold for left/right child selection
 * - left_child: Index of left child node (closer colors), -1 if leaf
 * - right_child: Index of right child node (farther colors), -1 if leaf
 */
typedef struct {{
    int16_t color_index;       /**< Index into color database */
    float median_distance;     /**< CIEDE2000 distance threshold */
    int16_t left_child;        /**< Left child index (-1 if none) */
    int16_t right_child;       /**< Right child index (-1 if none) */
}} vptree_node_t;

/**
 * @brief Number of nodes in the VP-Tree
 */
#define VPTREE_NODE_COUNT {num_nodes}

/**
 * @brief Root node index (always 0 for non-empty tree)
 */
#define VPTREE_ROOT_INDEX 0

/**
 * @brief Pre-computed VP-Tree stored in Flash
 * 
 * The tree is built using CIEDE2000 as the distance metric.
 * Search algorithm:
 * 1. Start at root node
 * 2. Calculate CIEDE2000 distance to current node's color
 * 3. Update best match if this distance is smaller
 * 4. If distance <= median: search left child (closer colors)
 * 5. If distance > median - best_distance: also search right child
 * 6. Recurse until all relevant branches are explored
 */
static const vptree_node_t vptree_nodes[VPTREE_NODE_COUNT] = {{
'''.format(
        num_colors=len(colors),
        num_nodes=len(node_list),
        memory_kb=len(node_list) * 10 / 1024.0  # 10 bytes per node (2+4+2+2)
    )
    
    for i, node in enumerate(node_list):
        header_content += f'    /* Node {i:4d} */ {{{node.color_index:5d}, {node.median_distance:10.4f}f, {node.left_child:5d}, {node.right_child:5d}}}'
        if i < len(node_list) - 1:
            header_content += ','
        header_content += '\n'
    
    header_content += '''}};

#ifdef __cplusplus
}}
#endif

#endif /* VPTREE_DATA_H */
'''
    
    with open(output_path, 'w', encoding='utf-8') as f:
        f.write(header_content)
    
    print(f"Generated {output_path} with {len(node_list)} VP-Tree nodes")
    print(f"Flash memory usage: {len(node_list) * 10 / 1024.0:.2f} KB")


def main():
    parser = argparse.ArgumentParser(
        description='Generate VP-Tree for color matching in Flash memory.'
    )
    parser.add_argument(
        '--input', '-i',
        default='xkcd_colors.json',
        help='Input JSON file (default: xkcd_colors.json)'
    )
    parser.add_argument(
        '--output', '-o',
        default=None,
        help='Output C++ header file (default: ../main/vptree_data.h)'
    )
    
    args = parser.parse_args()
    
    # Default paths
    script_dir = os.path.dirname(os.path.abspath(__file__))
    if args.output is None:
        args.output = os.path.join(script_dir, '..', 'main', 'vptree_data.h')
    
    if not os.path.isabs(args.input):
        args.input = os.path.join(script_dir, args.input)
    
    # Load colors
    print(f"Loading colors from {args.input}...")
    colors = load_colors_from_json(args.input)
    print(f"Loaded {len(colors)} colors")
    
    # Build VP-Tree
    print("Building VP-Tree...")
    node_list: List[VPTreeNode] = []
    indices = list(range(len(colors)))
    
    # Shuffle for better tree balance (optional, but helps with sorted input)
    random.seed(42)  # Deterministic for reproducible builds
    random.shuffle(indices)
    
    root_idx = build_vptree(colors, indices, node_list)
    print(f"Built VP-Tree with {len(node_list)} nodes, root at index {root_idx}")
    
    # Verify tree integrity
    max_depth = 0
    def check_depth(idx: int, depth: int):
        nonlocal max_depth
        if idx == -1:
            return
        max_depth = max(max_depth, depth)
        node = node_list[idx]
        check_depth(node.left_child, depth + 1)
        check_depth(node.right_child, depth + 1)
    
    check_depth(root_idx, 0)
    print(f"Tree depth: {max_depth} (optimal for {len(colors)} colors: {math.ceil(math.log2(len(colors)))})")
    
    # Generate output file
    generate_vptree_header(colors, node_list, args.output)
    
    return 0


if __name__ == '__main__':
    sys.exit(main())
