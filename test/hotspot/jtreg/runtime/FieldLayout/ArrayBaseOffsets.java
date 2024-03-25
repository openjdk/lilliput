/*
 * Copyright Amazon.com Inc. or its affiliates. All rights reserved.
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

/*
 * @test id=with-coops-no-ccp-no-ucoh
 * @library /test/lib /
 * @requires vm.bits == "64"
 * @modules java.base/jdk.internal.misc
 * @build jdk.test.whitebox.WhiteBox
 * @run driver jdk.test.lib.helpers.ClassFileInstaller jdk.test.whitebox.WhiteBox
 * @run main/othervm -Xbootclasspath/a:. -XX:+UnlockDiagnosticVMOptions -XX:+WhiteBoxAPI -XX:+UseCompressedOops -XX:-UseCompressedClassPointers -XX:+UnlockExperimentalVMOptions -XX:-UseCompactObjectHeaders ArrayBaseOffsets
 */
/*
 * @test id=with-coops-with-ccp-no-ucoh
 * @library /test/lib /
 * @requires vm.bits == "64"
 * @requires vm.opt.UseCompressedClassPointers != false
 * @modules java.base/jdk.internal.misc
 * @build jdk.test.whitebox.WhiteBox
 * @run driver jdk.test.lib.helpers.ClassFileInstaller jdk.test.whitebox.WhiteBox
 * @run main/othervm -Xbootclasspath/a:. -XX:+UnlockDiagnosticVMOptions -XX:+WhiteBoxAPI -XX:+UseCompressedOops -XX:+UseCompressedClassPointers -XX:+UnlockExperimentalVMOptions -XX:-UseCompactObjectHeaders ArrayBaseOffsets
 */
/*
 * @test id=no-coops-no-ccp-no-ucoh
 * @library /test/lib /
 * @requires vm.bits == "64"
 * @modules java.base/jdk.internal.misc
 * @build jdk.test.whitebox.WhiteBox
 * @run driver jdk.test.lib.helpers.ClassFileInstaller jdk.test.whitebox.WhiteBox
 * @run main/othervm -Xbootclasspath/a:. -XX:+UnlockDiagnosticVMOptions -XX:+WhiteBoxAPI -XX:-UseCompressedOops -XX:-UseCompressedClassPointers -XX:+UnlockExperimentalVMOptions -XX:-UseCompactObjectHeaders ArrayBaseOffsets
 */
/*
 * @test id=no-coops-with-ccp-no-ucoh
 * @library /test/lib /
 * @requires vm.bits == "64"
 * @requires vm.opt.UseCompressedClassPointers != false
 * @modules java.base/jdk.internal.misc
 * @build jdk.test.whitebox.WhiteBox
 * @run driver jdk.test.lib.helpers.ClassFileInstaller jdk.test.whitebox.WhiteBox
 * @run main/othervm -Xbootclasspath/a:. -XX:+UnlockDiagnosticVMOptions -XX:+WhiteBoxAPI -XX:-UseCompressedOops -XX:+UseCompressedClassPointers -XX:+UnlockExperimentalVMOptions -XX:-UseCompactObjectHeaders ArrayBaseOffsets
 */
/*
 * @test id=with-coops-no-ccp-with-ucoh
 * @library /test/lib /
 * @requires vm.bits == "64"
 * @modules java.base/jdk.internal.misc
 * @build jdk.test.whitebox.WhiteBox
 * @run driver jdk.test.lib.helpers.ClassFileInstaller jdk.test.whitebox.WhiteBox
 * @run main/othervm -Xbootclasspath/a:. -XX:+UnlockDiagnosticVMOptions -XX:+WhiteBoxAPI -XX:+UseCompressedOops -XX:-UseCompressedClassPointers -XX:+UnlockExperimentalVMOptions -XX:+UseCompactObjectHeaders ArrayBaseOffsets
 */
/*
 * @test id=with-coops-with-ccp-with-ucoh
 * @library /test/lib /
 * @requires vm.bits == "64"
 * @requires vm.opt.UseCompressedClassPointers != false
 * @modules java.base/jdk.internal.misc
 * @build jdk.test.whitebox.WhiteBox
 * @run driver jdk.test.lib.helpers.ClassFileInstaller jdk.test.whitebox.WhiteBox
 * @run main/othervm -Xbootclasspath/a:. -XX:+UnlockDiagnosticVMOptions -XX:+WhiteBoxAPI -XX:+UseCompressedOops -XX:+UseCompressedClassPointers -XX:+UnlockExperimentalVMOptions -XX:+UseCompactObjectHeaders ArrayBaseOffsets
 */
/*
 * @test id=no-coops-no-ccp-with-ucoh
 * @library /test/lib /
 * @requires vm.bits == "64"
 * @modules java.base/jdk.internal.misc
 * @build jdk.test.whitebox.WhiteBox
 * @run driver jdk.test.lib.helpers.ClassFileInstaller jdk.test.whitebox.WhiteBox
 * @run main/othervm -Xbootclasspath/a:. -XX:+UnlockDiagnosticVMOptions -XX:+WhiteBoxAPI -XX:-UseCompressedOops -XX:-UseCompressedClassPointers -XX:+UnlockExperimentalVMOptions -XX:+UseCompactObjectHeaders ArrayBaseOffsets
 */
/*
 * @test id=no-coops-with-ccp-with-ucoh
 * @library /test/lib /
 * @requires vm.bits == "64"
 * @requires vm.opt.UseCompressedClassPointers != false
 * @modules java.base/jdk.internal.misc
 * @build jdk.test.whitebox.WhiteBox
 * @run driver jdk.test.lib.helpers.ClassFileInstaller jdk.test.whitebox.WhiteBox
 * @run main/othervm -Xbootclasspath/a:. -XX:+UnlockDiagnosticVMOptions -XX:+WhiteBoxAPI -XX:-UseCompressedOops -XX:+UseCompressedClassPointers -XX:+UnlockExperimentalVMOptions -XX:+UseCompactObjectHeaders ArrayBaseOffsets
 */
/*
 * @test id=32bit
 * @library /test/lib /
 * @requires vm.bits == "32"
 * @modules java.base/jdk.internal.misc
 * @build jdk.test.whitebox.WhiteBox
 * @run driver jdk.test.lib.helpers.ClassFileInstaller jdk.test.whitebox.WhiteBox
 * @run main/othervm -Xbootclasspath/a:. -XX:+UnlockDiagnosticVMOptions -XX:+WhiteBoxAPI ArrayBaseOffsets
 */

import jdk.internal.misc.Unsafe;

import java.lang.management.ManagementFactory;
import java.lang.management.RuntimeMXBean;
import java.util.List;

import jdk.test.lib.Asserts;
import jdk.test.lib.Platform;
import jdk.test.whitebox.WhiteBox;

public class ArrayBaseOffsets {

    static final long INT_OFFSET;
    static final int  INT_ARRAY_OFFSET;
    static final int  LONG_ARRAY_OFFSET;
    static {
        WhiteBox WB = WhiteBox.getWhiteBox();
        if (!Platform.is64bit() || WB.getBooleanVMFlag("UseCompactObjectHeaders")) {
            INT_OFFSET = 8;
            INT_ARRAY_OFFSET = 12;
            LONG_ARRAY_OFFSET = 16;
        } else if (WB.getBooleanVMFlag("UseCompressedClassPointers")) {
            INT_OFFSET = 12;
            INT_ARRAY_OFFSET = 16;
            LONG_ARRAY_OFFSET = 16;
        } else {
            INT_OFFSET = 16;
            INT_ARRAY_OFFSET = 20;
            LONG_ARRAY_OFFSET = 24;
        }
    }

    static public void main(String[] args) {
        Unsafe unsafe = Unsafe.getUnsafe();
        Asserts.assertEquals(unsafe.arrayBaseOffset(boolean[].class), INT_ARRAY_OFFSET,  "Misplaced boolean array base");
        Asserts.assertEquals(unsafe.arrayBaseOffset(byte[].class),    INT_ARRAY_OFFSET,  "Misplaced byte    array base");
        Asserts.assertEquals(unsafe.arrayBaseOffset(char[].class),    INT_ARRAY_OFFSET,  "Misplaced char    array base");
        Asserts.assertEquals(unsafe.arrayBaseOffset(short[].class),   INT_ARRAY_OFFSET,  "Misplaced short   array base");
        Asserts.assertEquals(unsafe.arrayBaseOffset(int[].class),     INT_ARRAY_OFFSET,  "Misplaced int     array base");
        Asserts.assertEquals(unsafe.arrayBaseOffset(long[].class),    LONG_ARRAY_OFFSET, "Misplaced long    array base");
        Asserts.assertEquals(unsafe.arrayBaseOffset(float[].class),   INT_ARRAY_OFFSET,  "Misplaced float   array base");
        Asserts.assertEquals(unsafe.arrayBaseOffset(double[].class),  LONG_ARRAY_OFFSET, "Misplaced double  array base");
        boolean narrowOops = System.getProperty("java.vm.compressedOopsMode") != null ||
                             !Platform.is64bit();
        int expectedObjArrayOffset = narrowOops ? INT_ARRAY_OFFSET : LONG_ARRAY_OFFSET;
        Asserts.assertEquals(unsafe.arrayBaseOffset(Object[].class),  expectedObjArrayOffset, "Misplaced object  array base");
    }
}
