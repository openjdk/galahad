/*
 * Copyright (c) 2001, 2024, Oracle and/or its affiliates. All rights reserved.
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

package nsk.jdi.Event._itself_;

import com.sun.jdi.Field;
import com.sun.jdi.ReferenceType;
import com.sun.jdi.VirtualMachine;
import com.sun.jdi.request.AccessWatchpointRequest;
import com.sun.jdi.request.EventRequestManager;
import com.sun.jdi.VMDisconnectedException;
import com.sun.jdi.VMMismatchException;
import com.sun.jdi.event.*;

import java.util.Iterator;
import java.util.List;
import java.io.*;

import jdk.test.lib.Utils;
import nsk.share.*;
import nsk.share.jpda.*;
import nsk.share.jdi.*;

/**
 * The test checks up the JDI interface
 * <b>com.sun.jdi.event.Event.</b>
 * A debugger part of
 * the test creates several <code>AccessWatchpointRequest</code>s, and
 * verifies that all appropriate events have been generated by a target
 * VM. Then, the debugger removes one request via
 * <code>deleteEventRequest()</code> call. Finally, the test checks
 * that the event corresponding to the deleted request, are not generated.
 */
public class event001 {
    public static final int PASSED = 0;
    public static final int FAILED = 2;
    public static final int JCK_STATUS_BASE = 95;
    static final String DEBUGGEE_CLASS =
        "nsk.jdi.Event._itself_.event001t";
    static final String COMMAND_READY = "ready";
    static final String COMMAND_QUIT = "quit";

    static final int FLDS_NUM = 3;
    static final String COMMAND_RUN[] = {
        "run1", "run2", "run3"
    };
    static final String DEBUGGEE_FLDS[][] = {
        {"byte", "byteFld"},
        {"short", "shortFld"},
        {"int", "intFld"}
    };
    private volatile int accFCount[] = {
        0, 0, 0
    };
    private int DEL_NUM = 1; // deleted request number

    private ArgumentHandler argHandler;
    private Log log;
    private IOPipe pipe;
    private Debugee debuggee;
    private VirtualMachine vm;
    private EventListener elThread;
    private volatile AccessWatchpointRequest awpRequest[];
    private volatile int tot_res = PASSED;

// for notification a main thread about received events
    private Object gotEvent = new Object();

    public static void main (String argv[]) {
        int result = run(argv,System.out);
        if (result != 0) {
            throw new RuntimeException("TEST FAILED with result " + result);
        }
    }

    public static int run(String argv[], PrintStream out) {
        return new event001().runIt(argv, out);
    }

    private int runIt(String args[], PrintStream out) {
        argHandler = new ArgumentHandler(args);
        log = new Log(out, argHandler);
        Binder binder = new Binder(argHandler, log);
        ReferenceType rType;
        List fields;
        String cmd;

        debuggee = binder.bindToDebugee(DEBUGGEE_CLASS);
        pipe = debuggee.createIOPipe();
        debuggee.redirectStderr(log, "event001t.err> ");
        vm = debuggee.VM();
        EventRequestManager erManager = vm.eventRequestManager();
        debuggee.resume();
        cmd = pipe.readln();
        if (!cmd.equals(COMMAND_READY)) {
            log.complain("TEST BUG: unknown debuggee's command: "
                + cmd);
            tot_res = FAILED;
            return quitDebuggee();
        }

        if ( !vm.canWatchFieldAccess() ) {
            log.display("  TEST CANCELLED due to:  vm.canWatchFieldAccess() == false");
            return quitDebuggee();
        }

// Create testing requests
        if ((rType = debuggee.classByName(DEBUGGEE_CLASS)) == null) {
            log.complain("TEST FAILURE: Method Debugee.classByName() returned null");
            tot_res = FAILED;
            return quitDebuggee();
        }
        try {
            fields = rType.allFields();
        } catch (Exception e) {
            log.complain("TEST FAILURE: allFields: " + e);
            tot_res = FAILED;
            return quitDebuggee();
        }
        if (createRequests(erManager, fields) == FAILED) {
            tot_res = FAILED;
            return quitDebuggee();
        }

// Create a thread listening JDI events
        elThread = new EventListener();
        elThread.setPriority(Thread.NORM_PRIORITY + 2);
        synchronized(gotEvent) {
            elThread.start();

// Check all requested AccessWatchpointRequest events
            for (int i=0; i<FLDS_NUM; i++) {
                awpRequest[i].enable();
            }
            log.display("\na) Getting all requested AccessWatchpointEvents...");
            if (checkEvents(-1) == FAILED) {
                tot_res = FAILED;
                return quitDebuggee();
            }

            log.display("\nb) Getting AccessWatchpointEvents without request #"
                + DEL_NUM + "...");
// Remove one AccessWatchpointRequest
            log.display("\nRemoving AccessWatchpointRequest #"
                + DEL_NUM + " for the debuggee's field:\n\t"
                + DEBUGGEE_FLDS[DEL_NUM][0] + " " + DEBUGGEE_FLDS[DEL_NUM][1]);
            try {
                erManager.deleteEventRequest(awpRequest[DEL_NUM]);
            } catch (VMMismatchException e) {
                log.complain("TEST FAILED: EventRequestManager.deleteEventRequest: caught"
                    + e);
                e.printStackTrace();
                tot_res = FAILED;
                return quitDebuggee();
            }

// Check remained AccessWatchpointRequests
            if (checkEvents(DEL_NUM) == FAILED) {
                tot_res = FAILED;
                return quitDebuggee();
            }

// Finish the test
            for (int i=0; i<FLDS_NUM; i++) {
/* NOTE: for the jdk1.4: an InvalidRequestStateException could be
         checked for deleted request, but for the jdk1.3.x
         the InvalidRequestStateException is unspecified.
         So, I'm forced to omit disabling of deleted request */
                if (i != DEL_NUM)
                    awpRequest[i].disable();
            }
        }
        return quitDebuggee();
    }

    private int createRequests(EventRequestManager erManager,
            List fields) {
        Field fld = null;
        int i = 0;

        awpRequest =
            new AccessWatchpointRequest[FLDS_NUM];
        for (i=0; i<FLDS_NUM; i++) {
            boolean notFound = true;
            Iterator iter = fields.iterator();
            while (iter.hasNext()) {
                fld = (Field) iter.next();
                if (fld.name().equals(DEBUGGEE_FLDS[i][1]) &&
                    fld.typeName().equals(DEBUGGEE_FLDS[i][0])) {
                    log.display("Creating AccessWatchpointRequest for the debuggee's field:\n\t"
                        + fld.typeName() + " " + fld.name());
                    try {
                        awpRequest[i] = erManager.createAccessWatchpointRequest(fld);
                    } catch (Exception e) {
                        log.complain("TEST FAILED: createAccessWatchpointRequest: caught "
                            + e);
                        return FAILED;
                    }
                    notFound = false;
                    break;
                }
            }
            if (notFound) {
                log.complain("TEST FAILED: found unexpected debuggee's field:\n\t"
                    + fld.typeName() + " " + fld.name());
                return FAILED;
            }
        }
        return PASSED;
    }

    private int runTestCase(int i) {
        String token = null;

        log.display("\n" + (i+1) + ") Sending the command \""
            + COMMAND_RUN[i] + "\" to a debuggee");
        pipe.println(COMMAND_RUN[i]);

// wait for a requested event
        try {
            gotEvent.wait(Utils.adjustTimeout(argHandler.getWaitTime()*1000));
        } catch (InterruptedException e) {
            log.complain("TEST FAILURE: waiting for a requested AccessWatchpointEvent #"
                + i + ": caught " + e);
            e.printStackTrace();
            return FAILED;
        }
        log.display("Notification about the AccessWatchpointEvent #"
            + i + " received,\n\tor time has elapsed");

        if ((token = pipe.readln()) == null) {
            log.complain("TEST FAILURE: debuggee's reply is empty, probably due to the VM crash");
            return FAILED;
        }
        if (!token.equals(COMMAND_READY)) {
            log.complain("TEST BUG: unknown debuggee's command: " + token);
            return FAILED;
        }
        else log.display("Debuggee's reply received: "
            + token);
        return PASSED;
    }

    private int checkEvents(int excl_num) {
        for (int i=0; i<FLDS_NUM; i++) {
            accFCount[i] = 0;
            if (runTestCase(i) == FAILED)
                return FAILED;
            if (i == excl_num) {
                if (accFCount[i] != 0) {
                    log.complain("TEST FAILED: got AccessWatchpointEvent for the following field:\n\t"
                        + DEBUGGEE_FLDS[i][0] + " " + DEBUGGEE_FLDS[i][1]
                        + "\n\tbut corresponding request has been deleted via EventRequestManager.deleteEventRequest()");
                    tot_res = FAILED;
                } else
                    log.display("\nTEST PASSED: no event for the deleted AccessWatchpointRequest #"
                        + i);
            } else {
                if (accFCount[i] != 0) {
                    log.display("Got expected AccessWatchpointEvent for the following debuggee's field:\n\t"
                        + DEBUGGEE_FLDS[i][0] + " " + DEBUGGEE_FLDS[i][1]);
                } else {
                    log.complain("TEST FAILED: no AccessWatchpointEvent for the following field:\n\t"
                        + DEBUGGEE_FLDS[i][0] + " " + DEBUGGEE_FLDS[i][1]);
                    tot_res = FAILED;
                }
            }
        }
        return PASSED;
    }

    private int quitDebuggee() {
        if (elThread != null) {
            elThread.isConnected = false;
            try {
                if (elThread.isAlive())
                    elThread.join();
            } catch (InterruptedException e) {
                log.complain("TEST INCOMPLETE: caught InterruptedException "
                    + e);
                tot_res = FAILED;
            }
        }

        pipe.println(COMMAND_QUIT);
        debuggee.waitFor();
        int debStat = debuggee.getStatus();
        if (debStat != (JCK_STATUS_BASE + PASSED)) {
            log.complain("TEST FAILED: debuggee's process finished with status: "
                + debStat);
            tot_res = FAILED;
        } else
            log.display("Debuggee's process finished with status: "
                + debStat);

        return tot_res;
    }

    class EventListener extends Thread {
        public volatile boolean isConnected = true;

        public void run() {
            try {
                do {
                    EventSet eventSet = vm.eventQueue().remove(1000);
                    if (eventSet != null) { // there is not a timeout
                        EventIterator it = eventSet.eventIterator();
                        while (it.hasNext()) {
                            Event event = it.nextEvent();
                            if (event instanceof VMDeathEvent) {
                                tot_res = FAILED;
                                isConnected = false;
                                log.complain("TEST FAILED: caught unexpected VMDeathEvent");
                            } else if (event instanceof VMDisconnectEvent) {
                                tot_res = FAILED;
                                isConnected = false;
                                log.complain("TEST FAILED: caught unexpected VMDisconnectEvent");
                            } else {
                                log.display("EventListener: following JDI event occured: "
                                    + event.toString());

                                if (event instanceof AccessWatchpointEvent) {
                                    AccessWatchpointEvent awpEvent =
                                        (AccessWatchpointEvent) event;
                                    Field fld = awpEvent.field();
                                    boolean notFound = true;
                                    for (int i=0; i<FLDS_NUM; i++) {
                                        if (awpRequest[i].equals(event.request())) {
                                            log.display("EventListener: AccessWatchpointEvent for the debuggee's field #"
                                                + i + ":\n\t" + fld.typeName()
                                                + " " + fld.name());
                                            accFCount[i] += 1;
                                            notFound = false;
                                            log.display("EventListener: notifying about the received event #"
                                                + i);
                                            synchronized(gotEvent) {
                                                gotEvent.notify(); // notify the main thread
                                            }
                                            break;
                                        }
                                    }
                                    if (notFound) {
                                        log.complain("TEST FAILED: found in the received AccessWatchpointEvent\n\tunexpected debuggee's field "
                                            + fld.typeName() + " " + fld.name());
                                        tot_res = FAILED;
                                    }
                                }

                            }
                        }
                        if (isConnected) {
                            eventSet.resume();
                        }
                    }
                } while (isConnected);
            } catch (InterruptedException e) {
                tot_res = FAILED;
                log.complain("FAILURE in EventListener: caught unexpected "
                    + e);
            } catch (VMDisconnectedException e) {
                tot_res = FAILED;
                log.complain("FAILURE in EventListener: caught unexpected "
                    + e);
                e.printStackTrace();
            }
            log.display("EventListener: exiting");
        }
    }
}
