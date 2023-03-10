/*
 * Copyright (c) 2001, 2023, Oracle and/or its affiliates. All rights reserved.
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

package nsk.jdi.EventSet.resume;

import nsk.share.*;
import nsk.share.jdi.*;

/**
 * This class is used as debuggee application for the resume008 JDI test.
 */

public class resume008a {

    //----------------------------------------------------- template section

    static final int PASSED = 0;
    static final int FAILED = 2;
    static final int PASS_BASE = 95;

    static final String THREAD_NAME_PREFIX = "resume008-thread";

    static ArgumentHandler argHandler;
    static Log log;

    //--------------------------------------------------   log procedures

    private static void log1(String message) {
        log.display("**> debuggee: " + message);
    }

    private static void logErr(String message) {
        log.complain("**> debuggee: " + message);
    }

    //====================================================== test program

    static Thread thread0 = null;
    static Thread thread1 = null;
    static Thread thread2 = null;

    //------------------------------------------------------ common section

    static int exitCode = PASSED;

    static int testCase    = -1;
    static int instruction = 1;
    static int end         = 0;
                                   //    static int quit        = 0;
                                   //    static int continue    = 2;
    static int maxInstr    = 1;    // 2;

    static int lineForComm = 2;

    // Debugger sets a breakpoint here to track debuggee
    private static void methodForCommunication() {
        int i1 = instruction;
        int i2 = i1;
        int i3 = i2;
    }
    //----------------------------------------------------   main method

    public static void main (String argv[]) {

        argHandler = new ArgumentHandler(argv);
        log = argHandler.createDebugeeLog();

        log1("debuggee started!");

        label0:
        for (int i = 0; ; i++) {
            if (instruction > maxInstr) {
                logErr("ERROR: unexpected instruction: " + instruction);
                exitCode = FAILED;
                break ;
            }
            switch (i) {
    //------------------------------------------------------  section tested
            case 0:
                thread0 = JDIThreadFactory.newThread(new Threadresume008a(THREAD_NAME_PREFIX + 0));
                methodForCommunication();
                threadStart(thread0);
                thread1 = JDIThreadFactory.newThread(new Threadresume008a(THREAD_NAME_PREFIX + 1));
                // Wait for debugger to complete the first test case
                // before advancing to the 2nd breakpoint
                waitForTestCase(0);
                methodForCommunication();
                break;
            case 1:
                threadStart(thread1);
                thread2 = JDIThreadFactory.newThread(new Threadresume008a(THREAD_NAME_PREFIX + 2));
                methodForCommunication();
                break;
            case 2:
                threadStart(thread2);
            //-------------------------------------------------    standard end section
            default:
                instruction = end;
                methodForCommunication();
                break label0;
            }
        }
        log1("debuggee exits");
        System.exit(exitCode + PASS_BASE);
    }

    static Object waitnotifyObj = new Object();

    static int threadStart(Thread t) {
        synchronized (waitnotifyObj) {
            t.start();
            try {
                waitnotifyObj.wait();
            } catch ( Exception e) {
                exitCode = FAILED;
                logErr("       Exception : " + e );
                return FAILED;
            }
        }
        return PASSED;
    }
    // Synchronize with debugger progression.
    static void waitForTestCase(int t) {
        log1("waitForTestCase: " + t);
        while (testCase < t) {
            try {
                Thread.sleep(100);
            } catch (InterruptedException e) {
                // ignored
            }
        }
    }
    static class Threadresume008a extends NamedTask {

        public Threadresume008a(String threadName) {
            super(threadName);
        }
        public void run() {
            log1("  'run': enter  :: threadName == " + getName());
            synchronized (waitnotifyObj) {
                waitnotifyObj.notify();
            }
            log1("  'run': exit   :: threadName == " + getName());
            return;
        }
    }
}
