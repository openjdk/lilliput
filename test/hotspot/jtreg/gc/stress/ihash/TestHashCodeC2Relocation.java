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

package gc.stress.ihash;

/*
 * @test id=g1
 * @bug 8387150
 * @summary Identity hash code stays stable across GC relocation when the
 *          C2-compiled hashCode intrinsic recomputes an address-based hash.
 * @requires vm.gc.G1
 * @requires vm.compiler2.enabled
 * @requires vm.opt.UseCompactObjectHeaders == null | vm.opt.UseCompactObjectHeaders == true
 * @run main/othervm -XX:+UseCompactObjectHeaders -XX:+UseG1GC -Xms64m -Xmx64m
 *      gc.stress.ihash.TestHashCodeC2Relocation
 */

/*
 * @test id=parallel
 * @bug 8387150
 * @summary Identity hash code stays stable across GC relocation when the
 *          C2-compiled hashCode intrinsic recomputes an address-based hash.
 * @requires vm.gc.Parallel
 * @requires vm.compiler2.enabled
 * @requires vm.opt.UseCompactObjectHeaders == null | vm.opt.UseCompactObjectHeaders == true
 * @run main/othervm -XX:+UseCompactObjectHeaders -XX:+UseParallelGC -Xms64m -Xmx64m
 *      gc.stress.ihash.TestHashCodeC2Relocation
 */

/*
 * @test id=serial
 * @bug 8387150
 * @summary Identity hash code stays stable across GC relocation when the
 *          C2-compiled hashCode intrinsic recomputes an address-based hash.
 * @requires vm.gc.Serial
 * @requires vm.compiler2.enabled
 * @requires vm.opt.UseCompactObjectHeaders == null | vm.opt.UseCompactObjectHeaders == true
 * @run main/othervm -XX:+UseCompactObjectHeaders -XX:+UseSerialGC -Xms64m -Xmx64m
 *      gc.stress.ihash.TestHashCodeC2Relocation
 */

/*
 * @test id=shenandoah
 * @bug 8387150
 * @summary Identity hash code stays stable across GC relocation when the
 *          C2-compiled hashCode intrinsic recomputes an address-based hash.
 * @requires vm.gc.Shenandoah
 * @requires vm.compiler2.enabled
 * @requires vm.opt.UseCompactObjectHeaders == null | vm.opt.UseCompactObjectHeaders == true
 * @run main/othervm -XX:+UseCompactObjectHeaders -XX:+UseShenandoahGC -Xms64m -Xmx64m
 *      gc.stress.ihash.TestHashCodeC2Relocation
 */

/*
 * @test id=zgc
 * @bug 8387150
 * @summary Identity hash code stays stable across GC relocation when the
 *          C2-compiled hashCode intrinsic recomputes an address-based hash.
 * @requires vm.gc.Z
 * @requires vm.compiler2.enabled
 * @requires vm.opt.UseCompactObjectHeaders == null | vm.opt.UseCompactObjectHeaders == true
 * @run main/othervm -XX:+UseCompactObjectHeaders -XX:+UseZGC -Xms64m -Xmx64m
 *      gc.stress.ihash.TestHashCodeC2Relocation
 */

/**
 * Regression test for a C2 miscompilation of the {@code System.identityHashCode}
 * intrinsic under {@code -XX:+UseCompactObjectHeaders}.
 *
 * With compact headers there is no room in the 64-bit header to always store a
 * 31-bit identity hash. An object that has been hashed but not yet expanded by
 * the GC (hashctrl state {@code 0b01}) has its identity hash recomputed on every
 * read from the object's current heap address ({@code FastHash(address)}). This
 * is only correct while the object stays at the same address; when the GC
 * relocates the object it freezes the hash into a hidden field (state
 * {@code 0b11}).
 *
 * The C2 intrinsic inlined the {@code 0b01} recompute and materialized the raw
 * object address with a free-floating {@code CastP2X} (null control input).
 * Global Code Motion could hoist that materialization above a GC safepoint;
 * after a relocation the oop reference is updated but the already-materialized
 * raw address is not, so the recomputed hash was derived from the stale
 * pre-relocation address. The result: {@code System.identityHashCode} returned
 * inconsistent values for a single live object, violating the
 * {@code Object.hashCode()} stability contract.
 *
 * The reproducer hashes a fresh object, forces young-gen relocation via
 * allocation churn, and re-reads the hash. The "install after relocation"
 * ordering, warmed up so probe() is C2-compiled, is the shape that triggered the
 * miscompiled path. A correct VM reports zero mismatches.
 */
public class TestHashCodeC2Relocation {

    // Keep GC honest; defeat allocation elimination of the churn garbage.
    static volatile Object sink;

    static final long ITERS = 2_000_000L;

    /** Allocate short-lived garbage so the next young GC relocates our surviving object. */
    static void churn() {
        Object[] junk = new Object[64];
        for (int k = 0; k < 64; k++) {
            junk[k] = new byte[256];
        }
        sink = junk;
    }

    /**
     * @return a non-null description of the first observed violation, or null if stable.
     */
    static String probe(long iters, boolean installBefore) {
        for (long i = 0; i < iters; i++) {
            Object o = new Object();
            int h0, h1;
            if (installBefore) {
                h0 = System.identityHashCode(o);   // install hash, THEN relocate
                churn();
                h1 = System.identityHashCode(o);
            } else {
                churn();                           // relocate, THEN first read installs/loads hash
                h0 = System.identityHashCode(o);
                h1 = System.identityHashCode(o);
            }
            if (h1 != h0) {
                int settled = System.identityHashCode(o); // re-read returns the TRUE (frozen) hash
                return "identityHashCode returned inconsistent values for a live object"
                        + " at iter=" + i + ": h0=" + h0 + " h1=" + h1
                        + " settledReread=" + settled;
            }
        }
        return null;
    }

    public static void main(String[] args) {
        // Warm up with the safe ordering first so that probe() is compiled by C2
        // with the installBefore branch profiled -- the shape that exposed the bug.
        String warmup = probe(ITERS, true);
        if (warmup != null) {
            throw new RuntimeException("[warmup/installBefore] " + warmup);
        }

        // Detection run: read the identity hash only after relocation.
        String result = probe(ITERS, false);
        if (result != null) {
            throw new RuntimeException("[installAfter] " + result);
        }

        System.out.println("PASSED: identity hash codes stable across relocation");
    }
}
