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

package nsk.jdi.ReferenceType.allMethods;

import nsk.share.*;
import nsk.share.jpda.*;
import nsk.share.jdi.*;

import com.sun.jdi.*;
import java.util.*;
import java.io.*;

/**
 * This test checks the method <code>allMethods()</code>
 * of the JDI interface <code>ReferenceType</code> of com.sun.jdi package
 */

public class allmethods004 extends Log {
    static java.io.PrintStream out_stream;
    static boolean verbose_mode = false;  // test argument -vbs or -verbose switches to static
                                          // - for more easy failure evaluation

    /** The main class names of the debugger & debugee applications. */
    private final static String
        package_prefix = "nsk.jdi.ReferenceType.allMethods.",
//        package_prefix = "",    //  for DEBUG without package
        thisClassName = package_prefix + "allmethods004",
        debugeeName   = thisClassName + "a";

    /** Debugee's classes for check **/
    private final static String class_for_check = package_prefix + "allmethods004aInterfaceForCheck";


    public static void main (String argv[]) {
        int result = run(argv,System.out);
        if (result != 0) {
            throw new RuntimeException("TEST FAILED with result " + result);
        }

    }

    /**
     * JCK-like entry point to the test: perform testing, and
     * return exit code 0 (PASSED) or either 2 (FAILED).
     */
    public static int run (String argv[], PrintStream out) {
        out_stream = out;

        int v_test_result = new allmethods004().runThis(argv,out_stream);
        if ( v_test_result == 2/*STATUS_FAILED*/ ) {
            out_stream.println("\n==> nsk/jdi/ReferenceType/allMethods/allmethods004 test FAILED");
        }
        else {
            out_stream.println("\n==> nsk/jdi/ReferenceType/allMethods/allmethods004 test PASSED");
        }
        return v_test_result;
    }

    private void print_log_on_verbose(String message) {
        display(message);
    }

    /**
     * Non-static variant of the method <code>run(args,out)</code>
     */
    private int runThis (String argv[], PrintStream out) {
        ArgumentHandler argHandler = new ArgumentHandler(argv);
        verbose_mode = argHandler.verbose();

        if ( out_stream == null ) {
            out_stream = out;
        }

        out_stream.println("==> nsk/jdi/ReferenceType/allMethods/allmethods004 test LOG:");
        out_stream.println("==> test checks allMethods() method of ReferenceType interface ");
        out_stream.println("    of the com.sun.jdi package for class without any methods\n");

        String debugee_launch_command = debugeeName;
        if (verbose_mode) {
            logTo(out_stream);
        }

        Binder binder = new Binder(argHandler,this);
        Debugee debugee = binder.bindToDebugee(debugee_launch_command);
        IOPipe pipe = new IOPipe(debugee);

        debugee.redirectStderr(out);
        print_log_on_verbose("--> allmethods004: allmethods004a debugee launched");
        debugee.resume();

        String line = pipe.readln();
        if (line == null) {
            out_stream.println
                ("##> allmethods004: UNEXPECTED debugee's signal (not \"ready\") - " + line);
            return 2/*STATUS_FAILED*/;
        }
        if (!line.equals("ready")) {
            out_stream.println
                ("##> allmethods004: UNEXPECTED debugee's signal (not \"ready\") - " + line);
            return 2/*STATUS_FAILED*/;
        }
        else {
            print_log_on_verbose("--> allmethods004: debugee's \"ready\" signal recieved!");
        }

        out_stream.println
            ("--> allmethods004: check ReferenceType.allMethods() method for debugee's "
            + class_for_check + " class...");
        boolean class_not_found_error = false;
        boolean allMethods_method_error = false;
        int all_methods_number = 0;

        while ( true ) {
            ReferenceType refType = debugee.classByName(class_for_check);
            if (refType == null) {
                out_stream.println("##> allmethods004: Could NOT FIND class: " + class_for_check);
                class_not_found_error = true;
                break;
            }
            List<Method> all_methods_list = null;
            try {
                all_methods_list = refType.allMethods();
            }
            catch (Throwable thrown) {
                out_stream.println("##> allmethods004: FAILED: ReferenceType.allMethods() throws unexpected "
                    + thrown);
                allMethods_method_error = true;
                break;
            }
            all_methods_number = all_methods_list.size();
            if ( all_methods_number == 0 ) {
                break;
            }
            Method all_methods[] = new Method[all_methods_number];
            all_methods_list.toArray(all_methods);
            for (int i=0; i<all_methods_number; i++) {
                Method checked_method = all_methods[i];
                String checked_method_name = checked_method.name();
                String declaring_class_name = "declaring class NOT defined";
                try {
                    declaring_class_name = checked_method.declaringType().name();
                }
                catch (Throwable thrown) {
                }
                String full_checked_method_info = checked_method_name +"  (" + declaring_class_name + ")";
                out_stream.println
                    ("##> allmethods004: FAILED: unexpected found method: " + full_checked_method_info);
            }
            break;
        }

        int v_test_result = 0/*STATUS_PASSED*/;
        if ( class_not_found_error || allMethods_method_error ) {
            v_test_result = 2/*STATUS_FAILED*/;
        }

        if ( all_methods_number > 0 ) {
            out_stream.println
                ("##> allmethods004: UNEXPECTED all methods number = " + all_methods_number);
            v_test_result = 2/*STATUS_FAILED*/;
        }
        else {
            out_stream.println
                ("--> allmethods004: PASSED: returned list of methods is empty!");
        }

        print_log_on_verbose("--> allmethods004: waiting for debugee finish...");
        pipe.println("quit");
        debugee.waitFor();

        int status = debugee.getStatus();
        if (status != 0/*STATUS_PASSED*/ + 95/*STATUS_TEMP*/) {
            out_stream.println
                ("##> allmethods004: UNEXPECTED Debugee's exit status (not 95) - " + status);
            v_test_result = 2/*STATUS_FAILED*/;
        }
        else {
            print_log_on_verbose
                ("--> allmethods004: expected Debugee's exit status - " + status);
        }

        return v_test_result;
    }
}
