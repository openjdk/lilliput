/*
 * Copyright Amazon.com Inc. or its affiliates. All Rights Reserved.
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
 */

package gc.stress.ihash;

/*
 * @test id=Shenandoah
 * @bug 8387285
 * @summary An identity-hashed object whose size equals exactly one region must be
 *          allocated as humongous, so that the one-word hash expansion on a GC copy
 *          does not overflow a regular region.
 * @requires vm.gc.Shenandoah
 * @run main/othervm
 *      -XX:+UseCompactObjectHeaders -XX:+UseShenandoahGC
 *      -XX:+UnlockDiagnosticVMOptions -XX:+VerifyDuringGC -XX:+ShenandoahVerify
 *      -XX:-ExplicitGCInvokesConcurrent
 *      -Xms32m -Xmx32m
 *      -XX:+UnlockExperimentalVMOptions -XX:ShenandoahRegionSize=256K
 *      gc.stress.ihash.TestRegionSizedHash
 */

/*
 * @test id=Shenandoah-aggressive
 * @bug 8387285
 * @summary An identity-hashed object whose size equals exactly one region must be
 *          allocated as humongous, so that the one-word hash expansion on a GC copy
 *          does not overflow a regular region.
 * @requires vm.gc.Shenandoah
 * @run main/othervm
 *      -XX:+UseCompactObjectHeaders -XX:+UseShenandoahGC
 *      -XX:+UnlockDiagnosticVMOptions -XX:+VerifyDuringGC -XX:+ShenandoahVerify
 *      -XX:-ExplicitGCInvokesConcurrent
 *      -XX:ShenandoahGCHeuristics=aggressive
 *      -Xms32m -Xmx32m
 *      -XX:+UnlockExperimentalVMOptions -XX:ShenandoahRegionSize=256K
 *      gc.stress.ihash.TestRegionSizedHash
 */

/**
 * Regression test for the allocation-routing half of JDK-8387285.
 *
 * With compact object headers, an object whose size is exactly one region (its
 * allocation fills the region) would, if allocated as a regular object, overflow
 * that region when a GC copy injects an identity hash-code and grows it by one
 * word. ShenandoahFreeSet::allocate therefore classifies a fresh mutator object by
 * its potential expanded size: requires_humongous(size, may_expand_for_hash=true)
 * is align_object_size(size + 1) > RegionSizeWords, which is true for a
 * region-sized object, so it is placed in a humongous region up front.
 *
 * Without that routing flip, a region-sized object is allocated regular; once it is
 * identity-hashed and relocated by a STW Full GC, the expanded copy no longer fits
 * its destination region and Full GC fails:
 *   assert(_compact_point + obj_size <= _to_region->end()) failed: must fit
 *
 * The test allocates several region-sized, identity-hashed byte[] arrays at the
 * minimum (256K) region size -- the only regime where the expanded size of a
 * single-region object crosses the region boundary -- fragments them, and forces
 * STW Full GC compaction (-XX:-ExplicitGCInvokesConcurrent + System.gc()).
 */
public class TestRegionSizedHash {

    static final int REGION_SIZE = 256 * 1024;

    // byte[] allocation size (with compact headers) = align_up(8 + length, 8).
    // length = REGION_SIZE - 8 yields an allocation of exactly one region.
    static byte[] oneRegion() {
        return new byte[REGION_SIZE - 8];
    }

    static final int COUNT = 8;

    static Object[] keep;

    public static void main(String[] args) {
        keep = new Object[COUNT];
        int[] hashes = new int[COUNT];
        for (int i = 0; i < COUNT; i++) {
            byte[] o = oneRegion();
            hashes[i] = System.identityHashCode(o); // hash -> must expand on copy
            keep[i] = o;
        }

        // Fragment so Full GC slides the surviving region-sized objects.
        keep[1] = null;
        keep[3] = null;
        keep[5] = null;

        // STW Full GC compaction. Without the routing flip the relocated, expanded
        // region-sized objects overflow their destination region and Full GC fails.
        System.gc();

        // Validate survivors are intact across expansion/relocation.
        for (int i = 0; i < COUNT; i++) {
            byte[] o = (byte[]) keep[i];
            if (o == null) {
                continue;
            }
            if (System.identityHashCode(o) != hashes[i]) {
                throw new RuntimeException("hash mismatch at " + i);
            }
        }
        System.out.println("OK");
    }
}
