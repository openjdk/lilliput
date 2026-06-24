/*
 * Copyright (c) 2026, Amazon.com, Inc. or its affiliates. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 *
 */

/*
 * @test id=parallel
 * @bug 8387311
 * @summary Hashing a >2GB array and relocating it must not overflow the hash
 *          offset and corrupt the heap (compact object headers).
 * @library /test/lib
 * @library /
 * @requires vm.gc.Parallel
 * @requires vm.opt.UseCompactObjectHeaders == null | vm.opt.UseCompactObjectHeaders == true
 * @requires os.maxMemory >= 4G
 * @requires sun.arch.data.model == "64"
 * @key stress
 * @run main/othervm/timeout=300 -XX:+UseCompactObjectHeaders -XX:+UseParallelGC -Xmx4g
 *      TestHashCodeLargeArray
 */

/*
 * @test id=serial
 * @bug 8387311
 * @summary Hashing a >2GB array and relocating it must not overflow the hash
 *          offset and corrupt the heap (compact object headers).
 * @library /test/lib
 * @library /
 * @requires vm.gc.Serial
 * @requires vm.opt.UseCompactObjectHeaders == null | vm.opt.UseCompactObjectHeaders == true
 * @requires os.maxMemory >= 4G
 * @requires sun.arch.data.model == "64"
 * @key stress
 * @run main/othervm/timeout=300 -XX:+UseCompactObjectHeaders -XX:+UseSerialGC -Xmx4g
 *      TestHashCodeLargeArray
 */

/*
 * @test id=g1
 * @bug 8387311
 * @summary Hashing a >2GB array and relocating it must not overflow the hash
 *          offset and corrupt the heap (compact object headers).
 * @library /test/lib
 * @library /
 * @requires vm.gc.G1
 * @requires vm.opt.UseCompactObjectHeaders == null | vm.opt.UseCompactObjectHeaders == true
 * @requires os.maxMemory >= 4G
 * @requires sun.arch.data.model == "64"
 * @key stress
 * @run main/othervm/timeout=300 -XX:+UseCompactObjectHeaders -XX:+UseG1GC -Xmx4g
 *      TestHashCodeLargeArray
 */

/*
 * @test id=shen
 * @bug 8387311
 * @summary Hashing a >2GB array and relocating it must not overflow the hash
 *          offset and corrupt the heap (compact object headers).
 * @library /test/lib
 * @library /
 * @requires vm.gc.Shenandoah
 * @requires vm.opt.UseCompactObjectHeaders == null | vm.opt.UseCompactObjectHeaders == true
 * @requires os.maxMemory >= 4G
 * @requires sun.arch.data.model == "64"
 * @key stress
 * @run main/othervm/timeout=300 -XX:+UseCompactObjectHeaders -XX:+UseShenandoahGC -Xmx4g
 *      TestHashCodeLargeArray
 */

/*
 * @test id=zgc
 * @bug 8387311
 * @summary Hashing a >2GB array and relocating it must not overflow the hash
 *          offset and corrupt the heap (compact object headers).
 * @library /test/lib
 * @library /
 * @requires vm.gc.Z
 * @requires vm.opt.UseCompactObjectHeaders == null | vm.opt.UseCompactObjectHeaders == true
 * @requires os.maxMemory >= 4G
 * @requires sun.arch.data.model == "64"
 * @key stress
 * @run main/othervm/timeout=300 -XX:+UseCompactObjectHeaders -XX:+UseZGC -Xmx4g
 *      TestHashCodeLargeArray
 */

import jtreg.SkippedException;

/*
 * Regression test for an integer-overflow in ArrayKlass::hash_offset_in_bytes
 * under -XX:+UseCompactObjectHeaders.
 *
 * With compact headers an array's identity hash is stored in a hidden word that
 * the GC appends right after the elements when it "expands" the (hashed but not
 * yet expanded) object. The offset of that word was computed as
 * {@code base_offset + (length << log2_element_size)} in 32-bit int. For a
 * multi-byte element array of ~2GB or more this overflows to a negative value.
 * The GC then writes the 4-byte hash at a sign-extended negative displacement
 * from the object base, outside the array, which is a SIGSEGV or, worse, silent
 * corruption of a neighboring heap object.
 *
 * The test allocates a long[] just past the 2GB element-byte boundary
 * (length 2^28 + 8, whose payload 8*(2^28+8) makes the would-be hash offset
 * exceed INT_MAX), installs its identity hash, forces several relocating GC
 * cycles, and verifies the array contents and identity hash survive. On the
 * buggy VM this crashes (or silently corrupts a neighbor) during the first GC
 * that relocates the array.
 *
 * A long[] (8-byte elements) is used rather than an int[] so the offset crosses
 * INT_MAX at half the element count, keeping the heap footprint near 2GB while
 * still exercising the multi-byte-element overflow path.
 *
 * If the host cannot supply enough memory to allocate the array, the test ends
 * with a SkippedException so jtreg records SKIPPED (not PASSED). A passing
 * result must mean the relocating path was actually exercised.
 */
public class TestHashCodeLargeArray {

    // 2^28 + 8 longs. payload = 8 * (2^28 + 8) bytes > INT_MAX, so the hash
    // word offset (base + payload) overflows a 32-bit int in the buggy code.
    static final int LEN = 268435456 + 8;

    static volatile Object sink;

    public static void main(String[] args) {
        long[] a;
        try {
            a = new long[LEN];
        } catch (OutOfMemoryError e) {
            throw new SkippedException("Not enough heap to allocate the array");
        }

        // Touch first/last element so corruption of the tail is detectable.
        a[0] = 0x1111111111111111L;
        a[LEN - 1] = 0x2222222222222222L;

        // Install identity hash. The mark word becomes "hashed, not expanded".
        int h0 = System.identityHashCode(a);

        // Force relocation: the compacting GC expands the object and writes the
        // hash word at hash_offset_in_bytes(). This is where the overflow bites.
        for (int round = 0; round < 5; round++) {
            // Short-lived garbage to provoke promotion and compaction.
            for (int i = 0; i < 64; i++) {
                sink = new byte[1 << 20];
            }
            System.gc();

            int h1 = System.identityHashCode(a);
            if (h1 != h0) {
                throw new RuntimeException("identity hash changed across GC: h0=" + h0 + " h1=" + h1);
            }
            if (a.length != LEN || a[0] != 0x1111111111111111L || a[LEN - 1] != 0x2222222222222222L) {
                throw new RuntimeException("array contents corrupted after GC round " + round
                        + ": length=" + a.length + " first=" + Long.toHexString(a[0])
                        + " last=" + Long.toHexString(a[LEN - 1]));
            }
        }

        System.out.println("PASSED: large-array identity hash stable across relocation");
    }
}
