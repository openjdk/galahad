/*
 * Copyright (c) 2000, 2024, Oracle and/or its affiliates. All rights reserved.
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

package nsk.jdi.Argument.isValid;

import java.io.PrintStream;
import java.io.Serializable;

import java.util.Map;
import java.util.Set;
import java.util.List;
import java.util.Iterator;
import java.util.NoSuchElementException;

import com.sun.jdi.VirtualMachineManager;
import com.sun.jdi.Bootstrap;
import com.sun.jdi.connect.Connector;
import com.sun.jdi.connect.Connector.Argument;
import com.sun.jdi.connect.Connector.BooleanArgument;

/**
 * The test for the implementation of an object of the type <BR>
 * Connector.Argument. <BR>
 * <BR>
 * The test checks up that results of the method <BR>
 * <code>com.sun.jdi.connect.Connector.Argument.isValid()</code> <BR>
 * complies with its specification, that is,                            <BR>
 * "Returns: true if the value is valid to be used in setValue(String)" <BR>
 * in case when Argument implements StringArgument                      <BR>
 * and its parameter is a null-string.                                  <BR>
 * <BR>
 * In case of a wrong boolean value returned, <BR>
 * the test produces the return value 97 and <BR>
 * a corresponding error message. <BR>
 * Otherwise, the test is passed and produces <BR>
 * the return value 95 and no message. <BR>
 */


public class isvalid003 {

    public static void main(String argv[]) {
        int result = run(argv,System.out);
        if (result != 0) {
            throw new RuntimeException("TEST FAILED with result " + result);
        }
    }

    public static int run(String argv[], PrintStream out) {

        int exitCode  = 0;
        int exitCode0 = 0;
        int exitCode2 = 2;
//
        String sErr1 =  "WARNING\n" +
                        "Method tested: " +
                        "jdi.Connector.Argument.isValid\n" ;
//
        String sErr2 =  "ERROR\n" +
                        "Method tested: " +
                        "jdi.Connector.Argument.isValid\n" ;

        VirtualMachineManager vmm = Bootstrap.virtualMachineManager();

        List connectorsList = vmm.allConnectors();
        Iterator connectorsListIterator = connectorsList.iterator();
//
        Connector.Argument argument = null;
        Connector.StringArgument stringArg = null;

        for ( ; ; ) {
            try {
                Connector connector =
                (Connector) connectorsListIterator.next();

                Map defaultArguments = connector.defaultArguments();
                Set keyset     = defaultArguments.keySet();
                int keysetSize = defaultArguments.size();
                Iterator  keysetIterator = keyset.iterator();

                for ( ; ; ) {
                    try {
                        String argName = (String) keysetIterator.next();

                        try {
                            argument = (Connector.Argument)
                                       defaultArguments.get(argName);
//
                            try {
                                stringArg = (Connector.StringArgument)
                                             defaultArguments.get(argName);
                                break ;
                            } catch ( ClassCastException e) {
                            }
                        } catch ( ClassCastException e) {
                        }
                    } catch ( NoSuchElementException e) {
                        break ;
                    }
                }
//
                if (stringArg != null) {
                    break ;
                }
            } catch ( NoSuchElementException e) {
                out.println(sErr1 +
                    "no Connector with StringArgument found\n");
                return exitCode0;
            }
        }

        String sNull = null;
        try {
            if (!argument.isValid(sNull)) {
                exitCode = exitCode2;
                out.println(sErr2 +
                          "check: isValid(sNull)\n" +
//
                          "error: returned value != true\n");
            }
        } catch ( NullPointerException e ) {
            exitCode = exitCode2;
            out.println(sErr2 +
                      "check: isValid(sNull)\n" +
                      "error: NullPointerException\n");
        }

        if (exitCode != exitCode0)
            out.println("TEST FAILED");

        return exitCode;
    }
}
