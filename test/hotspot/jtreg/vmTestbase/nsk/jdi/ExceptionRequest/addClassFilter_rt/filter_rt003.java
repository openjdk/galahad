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

package nsk.jdi.ExceptionRequest.addClassFilter_rt;

import nsk.share.*;
import nsk.share.jpda.*;
import nsk.share.jdi.*;

import com.sun.jdi.*;
import com.sun.jdi.event.*;
import com.sun.jdi.request.*;

import java.util.*;
import java.io.*;

/**
 * The test for the implementation of an object of the type
 * ExceptionRequest.
 *
 * The test checks that results of the method
 * <code>com.sun.jdi.ExceptionRequest.addClassFilter(ReferenceType)</code>
 * complies with its spec.
 *
 * The test checks up on the following assertion:
 *    Restricts the events generated by this request to be
 *    the preparation of the given reference type and any subtypes.
 * The cases to test include re-invocations of the method
 * addClassFilter() on the same ExceptionRequest object.
 * There are two ExceptionRequests to check as follows:
 * (1) For ExceptionRequest2, both invocations are with different
 *     ReferenceTypes restricting one Exception event to two classes.
 *     The test expects no Exception event will be received.
 * (2) For ExceptionRequest1, both invocations are with the same
 *     ReferenceType restricting one Exception event to one class.
 *     The test expects this Exception event will be received.
 *
 * The test works as follows.
 * - The debugger resumes the debuggee and waits for the BreakpointEvent.
 * - The debuggee creates two threads, thread1 and thread2
 *   (thread1 will invoke methods throwing NullPointerException in
 *   the super-class TestClass10 and its sub-class TestClass11),
 *   and invokes the methodForCommunication to be suspended and
 *   to inform the debugger with the event.
 * - Upon getting the BreakpointEvent, the debugger
 *   - gets ReferenceTypes 1&2 for the Classes to filter,
 *   - sets up two ExceptionRequests 1&2,
 *   - double restricts ExceptionRequest1 to the RefTypes 1 and 1,
 *   - double restricts ExceptionRequest2 to the RefTypes 1 and 2,
 *   - resumes debuggee's main thread, and
 *   - waits for the event.
 * - The debuggee starts thread1 and thread2, one by one,
 *   to generate the events to be filtered.
 * - Upon getting the events, the debugger performs checks required.
 */

public class filter_rt003 extends TestDebuggerType1 {

    public static void main (String argv[]) {
        int result = run(argv,System.out);
        if (result != 0) {
            throw new RuntimeException("TEST FAILED with result " + result);
        }
    }

    public static int run (String argv[], PrintStream out) {
        debuggeeName = "nsk.jdi.ExceptionRequest.addClassFilter_rt.filter_rt003a";
        return new filter_rt003().runThis(argv, out);
    }

    private String testedClassName11 =
      "nsk.jdi.ExceptionRequest.addClassFilter_rt.filter_rt003aTestClass11";
    private String testedClassName21 =
      "nsk.jdi.ExceptionRequest.addClassFilter_rt.filter_rt003aTestClass21";

    protected void testRun() {

        EventRequest  eventRequest1      = null;
        EventRequest  eventRequest2      = null;

        String        property1          = "ExceptionRequest1";
        String        property2          = "ExceptionRequest2";

        ReferenceType testClassReference11 = null;
        ReferenceType testClassReference21 = null;

        for (int i = 0; ; i++) {

            if (!shouldRunAfterBreakpoint()) {
                vm.resume();
                break;
            }

            display(":::::: case: # " + i);

            switch (i) {

                case 0:
                testClassReference11 = (ReferenceType)debuggee.classByName(testedClassName11);
                testClassReference21 = (ReferenceType)debuggee.classByName(testedClassName21);

                eventRequest1 = setting21ExceptionRequest(null, testClassReference11,
                                      EventRequest.SUSPEND_ALL, property1);

                eventRequest2 = setting21ExceptionRequest(null, testClassReference11,
                                      EventRequest.SUSPEND_ALL, property2);

                ((ExceptionRequest) eventRequest1).addClassFilter(testClassReference11);
                ((ExceptionRequest) eventRequest2).addClassFilter(testClassReference21);
                display("......waiting for MethodEntryEvent in expected thread");
                Event newEvent = eventHandler.waitForRequestedEvent(new EventRequest[]{eventRequest1, eventRequest2}, waitTime, false);

                if ( !(newEvent instanceof ExceptionEvent)) {
                    setFailedStatus("ERROR: new event is not ExceptionEvent");
                } else {

                    String property = (String) newEvent.request().getProperty("number");
                    display("       got new ExceptionEvent with property 'number' == " + property);

                    if ( !property.equals(property1) ) {
                        setFailedStatus("ERROR: property is not : " + property1);
                    }

                    ReferenceType refType = ((ExceptionEvent)newEvent).location().declaringType();
                    if (!refType.equals(testClassReference11)) {
                        setFailedStatus("Received unexpected declaring type of the event: " + refType.name() +
                            "\n\texpected one: " + testClassReference11.name());
                    }
                }
                vm.resume();
                break;


                default:
                throw new Failure("** default case 2 **");
            }
        }
        return;
    }


    private ExceptionRequest setting21ExceptionRequest ( ThreadReference thread,
                                                         ReferenceType   testedClass,
                                                         int             suspendPolicy,
                                                         String          property       ) {
        try {
            display("......setting up ExceptionRequest:");
            display("       thread: " + thread + "; class: " + testedClass + "; property: " + property);

            ExceptionRequest
            excr = eventRManager.createExceptionRequest(null, true, true);
            excr.putProperty("number", property);
            if (thread != null)
                excr.addThreadFilter(thread);
            excr.addClassFilter(testedClass);
            excr.setSuspendPolicy(suspendPolicy);

            display("      ExceptionRequest has been set up");
            return excr;
        } catch ( Exception e ) {
            throw new Failure("** FAILURE to set up ExceptionRequest **");
        }
    }

}
