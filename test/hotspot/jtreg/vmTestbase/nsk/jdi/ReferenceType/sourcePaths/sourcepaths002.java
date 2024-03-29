/*
 * Copyright (c) 2002, 2024, Oracle and/or its affiliates. All rights reserved.
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

package nsk.jdi.ReferenceType.sourcePaths;

import com.sun.jdi.*;
import com.sun.jdi.event.*;
import com.sun.jdi.request.*;

import java.util.*;
import java.io.*;

import nsk.share.*;
import nsk.share.jpda.*;
import nsk.share.jdi.*;

/**
 * The test checks that the JDI method
 * <code>com.sun.jdi.ReferenceType.sourcePaths()</code>
 * properly throws <i>AbsentInformationException</i> - if the source
 * paths are not known.<p>
 *
 * The test exercises the following assertion:<br>
 * <i>"For arrays (ArrayType) and primitive classes,
 * AbsentInformationException is always thrown"</i>.<br>
 * Debugger part attempts to get paths to the sources corresponding
 * to the declaration types of several debuggee fields, which are:<br>
 * primitive classes themselves, and arrays of primitive types and classes.
 */
public class sourcepaths002 {
    static final String DEBUGGEE_CLASS =
        "nsk.jdi.ReferenceType.sourcePaths.sourcepaths002t";

    // debuggee source line where it should be stopped
    static final int DEBUGGEE_STOPATLINE = 52;

    static final int FLD_NUM = 24;
    // tested fields used to provoke the exception
    static final String DEBUGGEE_FLDS[] = {
        "boolCls",
        "byteCls",
        "charCls",
        "doubleCls",
        "floatCls",
        "intCls",
        "longCls",
        "shortCls",
        "boolArr",
        "byteArr",
        "charArr",
        "doubleArr",
        "floatArr",
        "intArr",
        "longArr",
        "shortArr",
        "boolClsArr",
        "byteClsArr",
        "charClsArr",
        "doubleClsArr",
        "floatClsArr",
        "intClsArr",
        "longClsArr",
        "shortClsArr"
    };

    static final String COMMAND_READY = "ready";
    static final String COMMAND_GO = "go";
    static final String COMMAND_QUIT = "quit";

    static final int DELAY = 500; // in milliseconds

    private ArgumentHandler argHandler;
    private Log log;
    private IOPipe pipe;
    private Debugee debuggee;
    private VirtualMachine vm;
    private volatile int tot_res = Consts.TEST_PASSED;
    private BreakpointRequest BPreq;
    private volatile boolean gotEvent = false;

    public static void main (String argv[]) {
        int result = run(argv,System.out);
        if (result != 0) {
            throw new RuntimeException("TEST FAILED with result " + result);
        }
    }

    public static int run(String argv[], PrintStream out) {
        return new sourcepaths002().runIt(argv, out);
    }

    private int runIt(String args[], PrintStream out) {
        argHandler = new ArgumentHandler(args);
        log = new Log(out, argHandler);
        Binder binder = new Binder(argHandler, log);

        debuggee = binder.bindToDebugee(DEBUGGEE_CLASS);
        pipe = debuggee.createIOPipe();
        vm = debuggee.VM();
        debuggee.redirectStderr(log, "sourcepaths002t.err> ");
        debuggee.resume();
        String cmd = pipe.readln();
        if (!cmd.equals(COMMAND_READY)) {
            log.complain("TEST BUG: unknown debuggee command: " + cmd);
            tot_res = Consts.TEST_FAILED;
            return quitDebuggee();
        }

        try {
            // debuggee main & dummy classes
            ReferenceType rType = debuggee.classByName(DEBUGGEE_CLASS);

            // suspend debuggee VM at breakpoint
            suspendAtBP(rType, DEBUGGEE_STOPATLINE);

            for (int i=0; i<FLD_NUM; i++) {
                Value val = rType.getValue(rType.fieldByName(DEBUGGEE_FLDS[i]));

                ReferenceType testedType = null;
                if (val.type() instanceof ArrayType)
                    testedType = ((ArrayReference) val).referenceType();
                else
                    testedType = ((ClassObjectReference) val).reflectedType();

                log.display("\n");
                for (int j=0; j<2; j++) {
                    String testCase =
                        new String((j==0)? "null" : "VirtualMachine.getDefaultStratum() value");
                    List srcPaths = null;

                    // Check the tested assersion
                    try {
                        log.display((i+1) + "." + (j+1)
                            + ") Trying to get paths to the source corresponding to the reference type \""
                            + testedType + "\"\tname: " + testedType.name()
                            + "\n\tusing " + testCase + " as a stratum parameter ...");
                        switch(j) {
                        case 0:
                            srcPaths = testedType.sourcePaths(null);
                            break;
                        case 1:
                            srcPaths = testedType.sourcePaths(vm.getDefaultStratum());
                            break;
                        }
                        log.complain("TEST FAILED: expected AbsentInformationException was not thrown"
                            + "\n\twhen attempted to get paths to the source corresponding to the reference type \""
                            + testedType + "\"\tname: " + testedType.name()
                            + "\n\tusing " + testCase + " as a stratum parameter"
                            + "\n\tThe paths are: " + srcPaths);
                        tot_res = Consts.TEST_FAILED;
                    } catch (AbsentInformationException ee) {
                        log.display("CHECK PASSED: caught expected " + ee);
                    } catch (Exception ue) {
                        ue.printStackTrace();
                        log.complain("TEST FAILED: caught unexpected "
                            + ue + "\n\tinstead of expected AbsentInformationException"
                            + "\n\twhen attempted to get paths to the source corresponding to the reference type \""
                            + testedType + "\"\tname: " + testedType.name()
                            + "\n\tusing " + testCase + " as a stratum parameter");
                        tot_res = Consts.TEST_FAILED;
                    }

                }
            }
        } catch (Exception e) {
            e.printStackTrace();
            log.complain("TEST FAILURE: caught unexpected exception: " + e);
            tot_res = Consts.TEST_FAILED;
        }

// Finish the test
        return quitDebuggee();
    }

    private BreakpointRequest setBP(ReferenceType refType, int bpLine) {
        EventRequestManager evReqMan =
            debuggee.getEventRequestManager();
        Location loc;

        try {
            List locations = refType.allLineLocations();
            Iterator iter = locations.iterator();
            while (iter.hasNext()) {
                loc = (Location) iter.next();
                if (loc.lineNumber() == bpLine) {
                    BreakpointRequest BPreq =
                        evReqMan.createBreakpointRequest(loc);
                    BPreq.setSuspendPolicy(EventRequest.SUSPEND_EVENT_THREAD);
                    log.display("created " + BPreq + "\n\tfor " + refType
                        + " ; line=" + bpLine);
                    return BPreq;
                }
            }
        } catch (Exception e) {
            e.printStackTrace();
            throw new Failure("setBP: caught unexpected exception: " + e);
        }
        throw new Failure("setBP: location corresponding debuggee source line "
            + bpLine + " not found");
    }

    private void suspendAtBP(ReferenceType rType, int bpLine) {

// Class containing a critical section which may lead to time out of the test
        class CriticalSection extends Thread {
            public volatile boolean waitFor = true;

            public void run() {
                try {
                    do {
                        EventSet eventSet = vm.eventQueue().remove(DELAY);
                        if (eventSet != null) { // it is not a timeout
                            EventIterator it = eventSet.eventIterator();
                            while (it.hasNext()) {
                                Event event = it.nextEvent();
                                if (event instanceof VMDisconnectEvent) {
                                    log.complain("TEST FAILED: unexpected VMDisconnectEvent");
                                    break;
                                } else if (event instanceof VMDeathEvent) {
                                    log.complain("TEST FAILED: unexpected VMDeathEvent");
                                    break;
                                } else if (event instanceof BreakpointEvent) {
                                    if (event.request().equals(BPreq)) {
                                        log.display("expected Breakpoint event occured: "
                                            + event.toString());
                                        gotEvent = true;
                                        return;
                                    }
                                } else
                                    log.display("following JDI event occured: "
                                        + event.toString());
                            }
                        }
                    } while(waitFor);
                    log.complain("TEST FAILED: no expected Breakpoint event");
                    tot_res = Consts.TEST_FAILED;
                } catch (Exception e) {
                    e.printStackTrace();
                    tot_res = Consts.TEST_FAILED;
                    log.complain("TEST FAILED: caught unexpected exception: " + e);
                }
            }
        }
/////////////////////////////////////////////////////////////////////////////

        BPreq = setBP(rType, bpLine);
        BPreq.enable();
        CriticalSection critSect = new CriticalSection();
        log.display("\nStarting potential timed out section:\n\twaiting "
            + (argHandler.getWaitTime())
            + " minute(s) for JDI Breakpoint event ...\n");
        critSect.start();
        pipe.println(COMMAND_GO);
        try {
            critSect.join((argHandler.getWaitTime())*60000);
            if (critSect.isAlive()) {
                critSect.waitFor = false;
                throw new Failure("timeout occured while waiting for Breakpoint event");
            }
        } catch (InterruptedException e) {
            critSect.waitFor = false;
            throw new Failure("TEST INCOMPLETE: InterruptedException occured while waiting for Breakpoint event");
        } finally {
            BPreq.disable();
        }
        log.display("\nPotential timed out section successfully passed\n");
        if (gotEvent == false)
            throw new Failure("unable to suspend debuggee thread at breakpoint");
    }

    private int quitDebuggee() {
        log.display("\nFinal resumption of the debuggee VM");
        vm.resume();
        pipe.println(COMMAND_QUIT);
        debuggee.waitFor();
        int debStat = debuggee.getStatus();
        if (debStat != (Consts.JCK_STATUS_BASE + Consts.TEST_PASSED)) {
            log.complain("TEST FAILED: debuggee process finished with status: "
                + debStat);
            tot_res = Consts.TEST_FAILED;
        } else
            log.display("\nDebuggee process finished with the status: "
                + debStat);

        return tot_res;
    }
}
