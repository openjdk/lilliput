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
 *
 */

/*
 * @test id=default
 * @summary Test for race between identityHashCode and evacuation with compact headers
 * @bug 8379910
 * @requires vm.gc.Shenandoah
 * @requires vm.debug
 * @requires vm.opt.UseCompactObjectHeaders == null | vm.opt.UseCompactObjectHeaders == true
 * @library /test/lib
 * @run main/othervm -XX:+UseCompactObjectHeaders -XX:+UseShenandoahGC
 *      -XX:ShenandoahGCHeuristics=aggressive
 *      -XX:+ShenandoahHashEvacBeforeCopy
 *      -XX:+ShenandoahVerify -Xms32m -Xmx32m
 *      TestHashCodeEvacRace
 */

/*
 * @test id=generational
 * @summary Test for race between identityHashCode and evacuation with compact headers
 * @bug 8379910
 * @requires vm.gc.Shenandoah
 * @requires vm.debug
 * @requires vm.opt.UseCompactObjectHeaders == null | vm.opt.UseCompactObjectHeaders == true
 * @library /test/lib
 * @run main/othervm -XX:+UseCompactObjectHeaders -XX:+UseShenandoahGC
 *      -XX:ShenandoahGCMode=generational
 *      -XX:+ShenandoahHashEvacBeforeCopy
 *      -XX:+ShenandoahVerify -Xms32m -Xmx32m
 *      TestHashCodeEvacRace
 */

/**
 * Regression test for a race between concurrent evacuation and
 * System.identityHashCode(). With compact object headers, objects that have
 * no internal gap for the identity hash need an extra word when evacuated
 * (hash expansion). The allocation size for the copy is determined from a
 * captured mark word. If a mutator thread hashes the original object between
 * the mark capture and the bulk copy, the copy picks up the updated mark
 * (is_hashed_not_expanded) while the allocation used the stale mark (no_hash).
 * initialize_hash_if_necessary() then writes the hash beyond the allocation,
 * corrupting the next object in the PLAB.
 *
 * The class IntHolder has layout [4-byte header][4-byte int] = 8 bytes with
 * compact headers, leaving no gap for the hash — so it requires expansion.
 *
 * ShenandoahHashEvacBeforeCopy widens the race window between mark capture
 * and object copy so that mutator threads can hash the original concurrently.
 *
 * The test separates object creation (producer threads) from hashing (hasher
 * threads). This ensures a steady supply of unhashed objects that GC may
 * begin evacuating, while hasher threads race to hash them.
 */
public class TestHashCodeEvacRace {

    // With compact headers: 4-byte header + 4-byte int = 8 bytes.
    // No room for the 4-byte identity hash inside the object — expansion required.
    static class IntHolder {
        int value;
        IntHolder(int v) { value = v; }
    }

    static final int NUM_OBJECTS = 50_000;
    static final int NUM_PRODUCERS = 2;
    static final int NUM_HASHERS = 4;
    static final int DURATION_MS = 10_000;

    // Shared array of live objects. Producers write fresh (unhashed) objects,
    // hashers read and hash them. GC evacuates them concurrently.
    static final IntHolder[] objects = new IntHolder[NUM_OBJECTS];

    static volatile boolean running = true;

    public static void main(String[] args) throws Exception {
        // Populate initial objects (unhashed).
        for (int i = 0; i < NUM_OBJECTS; i++) {
            objects[i] = new IntHolder(i);
        }

        // Producer threads: continuously replace objects with fresh unhashed ones.
        Thread[] producers = new Thread[NUM_PRODUCERS];
        for (int t = 0; t < NUM_PRODUCERS; t++) {
            final int tid = t;
            producers[t] = new Thread(() -> {
                int from = tid * (NUM_OBJECTS / NUM_PRODUCERS);
                int to = (tid + 1) * (NUM_OBJECTS / NUM_PRODUCERS);
                int idx = from;
                while (running) {
                    objects[idx] = new IntHolder(idx);
                    idx++;
                    if (idx >= to) idx = from;
                }
            });
            producers[t].setDaemon(true);
            producers[t].start();
        }

        // Hasher threads: continuously hash objects from the array.
        // They will encounter objects that are unhashed (freshly placed by producers)
        // and CAS their mark word — this races with GC evacuation.
        Thread[] hashers = new Thread[NUM_HASHERS];
        for (int t = 0; t < NUM_HASHERS; t++) {
            hashers[t] = new Thread(() -> {
                java.util.Random rng = new java.util.Random();
                while (running) {
                    int idx = rng.nextInt(NUM_OBJECTS);
                    IntHolder obj = objects[idx];
                    if (obj != null) {
                        System.identityHashCode(obj);
                    }
                }
            });
            hashers[t].setDaemon(true);
            hashers[t].start();
        }

        // Main thread: allocate garbage to trigger GC cycles.
        long deadline = System.currentTimeMillis() + DURATION_MS;
        while (System.currentTimeMillis() < deadline) {
            for (int i = 0; i < 100; i++) {
                byte[] garbage = new byte[4096];
            }
            Thread.yield();
        }

        running = false;
        for (Thread t : producers) t.join(5000);
        for (Thread t : hashers) t.join(5000);

        System.out.println("PASSED");
    }
}
