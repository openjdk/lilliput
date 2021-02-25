/*
 * Copyright (c) 2021, Oracle and/or its affiliates. All rights reserved.
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
 * @test
 * @run testng TestTableSwitch
 */

import org.testng.annotations.Test;

import java.lang.invoke.MethodHandle;
import java.lang.invoke.MethodHandles;
import java.lang.invoke.MethodType;
import java.util.function.IntConsumer;
import java.util.function.IntFunction;

import static org.testng.Assert.assertEquals;

public class TestTableSwitch {

    static final MethodHandle MH_IntConsumer_accept;

    static {
        try {
            MethodHandles.Lookup lookup = MethodHandles.lookup();
            MH_IntConsumer_accept = lookup.findVirtual(IntConsumer.class, "accept",
                    MethodType.methodType(void.class, int.class));
        } catch (ReflectiveOperationException e) {
            throw new ExceptionInInitializerError(e);
        }
    }

    public static MethodHandle simpleTestCase(String message) {
        return MethodHandles.dropArguments(MethodHandles.constant(String.class, message), 0, int.class);
    }

    @Test
    public void testNonVoidHandles() throws Throwable {
        MethodHandle mhSwitch = MethodHandles.tableSwitch(
            /* default: */ simpleTestCase("Default"),
            /* case 0: */  simpleTestCase("Case 1"),
            /* case 1: */  simpleTestCase("Case 2"),
            /* case 2: */  simpleTestCase("Case 3")
        );

        assertEquals((String) mhSwitch.invokeExact((int) -1), "Default");
        assertEquals((String) mhSwitch.invokeExact((int) 0), "Case 1");
        assertEquals((String) mhSwitch.invokeExact((int) 1), "Case 2");
        assertEquals((String) mhSwitch.invokeExact((int) 2), "Case 3");
        assertEquals((String) mhSwitch.invokeExact((int) 3), "Default");
    }

    @Test
    public void testVoidHandles() throws Throwable {
        IntFunction<MethodHandle> makeTestCase = expectedIndex -> {
            IntConsumer test = actualIndex -> assertEquals(actualIndex, expectedIndex);
            return MH_IntConsumer_accept.bindTo(test);
        };

        MethodHandle mhSwitch = MethodHandles.tableSwitch(
            /* default: */ makeTestCase.apply(-1),
            /* case 0: */  makeTestCase.apply(0),
            /* case 1: */  makeTestCase.apply(1),
            /* case 2: */  makeTestCase.apply(2)
        );

        mhSwitch.invokeExact((int) -1);
        mhSwitch.invokeExact((int) 0);
        mhSwitch.invokeExact((int) 1);
        mhSwitch.invokeExact((int) 2);
    }

    @Test(expectedExceptions = NullPointerException.class)
    public void testNullDefaultHandle() {
        MethodHandles.tableSwitch(null, simpleTestCase("test"));
    }

    @Test(expectedExceptions = NullPointerException.class)
    public void testNullCases() {
        MethodHandle[] cases = null;
        MethodHandles.tableSwitch(simpleTestCase("default"), cases);
    }

    @Test(expectedExceptions = NullPointerException.class)
    public void testNullCase() {
        MethodHandles.tableSwitch(simpleTestCase("default"), simpleTestCase("case"), null);
    }

    @Test(expectedExceptions = IllegalArgumentException.class,
          expectedExceptionsMessageRegExp = ".*Not enough cases.*")
    public void testNotEnoughCases() {
        MethodHandles.tableSwitch(simpleTestCase("default"), simpleTestCase("case"));
    }

    @Test(expectedExceptions = IllegalArgumentException.class,
          expectedExceptionsMessageRegExp = ".*Case actions must have int as leading parameter.*")
    public void testNotEnoughParameters() {
        MethodHandle empty = MethodHandles.empty(MethodType.methodType(void.class));
        MethodHandles.tableSwitch(empty, empty, empty);
    }

    @Test(expectedExceptions = IllegalArgumentException.class,
          expectedExceptionsMessageRegExp = ".*Case actions must have int as leading parameter.*")
    public void testNoLeadingIntParameter() {
        MethodHandle empty = MethodHandles.empty(MethodType.methodType(void.class, double.class));
        MethodHandles.tableSwitch(empty, empty, empty);
    }

    @Test(expectedExceptions = IllegalArgumentException.class,
          expectedExceptionsMessageRegExp = ".*Case actions must have the same type.*")
    public void testWrongCaseType() {
        // doesn't return a String
        MethodHandle wrongType = MethodHandles.empty(MethodType.methodType(void.class, int.class));
        MethodHandles.tableSwitch(simpleTestCase("default"), simpleTestCase("case"), wrongType);
    }

}
