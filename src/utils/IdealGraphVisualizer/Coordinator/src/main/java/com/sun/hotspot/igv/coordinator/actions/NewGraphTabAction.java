/*
 * Copyright (c) 2013, 2023, Oracle and/or its affiliates. All rights reserved.
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

package com.sun.hotspot.igv.coordinator.actions;

import org.openide.nodes.Node;
import org.openide.util.HelpCtx;
import org.openide.util.actions.CookieAction;

/**
 *
 * @author Thomas Wuerthinger
 */
public final class NewGraphTabAction extends CookieAction {

    @Override
    protected void performAction(Node[] activatedNodes) {
        NewGraphTabCookie c = activatedNodes[0].getCookie(NewGraphTabCookie.class);
        assert c != null;
        c.openNewTab();
    }

    @Override
    protected int mode() {
        return CookieAction.MODE_EXACTLY_ONE;
    }

    @Override
    protected boolean enable(Node[] activatedNodes) {
        boolean b = super.enable(activatedNodes);
        if (b) {
            assert activatedNodes.length == 1;
            NewGraphTabCookie c = activatedNodes[0].getCookie(NewGraphTabCookie.class);
            assert c != null;
            return c.isActive();
        }

        return false;
    }

    @Override
    public String getName() {
        return "Open in new tab";
    }

    @Override
    protected Class<?>[] cookieClasses() {
        return new Class<?>[]{
                NewGraphTabCookie.class
        };
    }

    @Override
    protected String iconResource() {
        return "com/sun/hotspot/igv/coordinator/images/graph.png";
    }

    @Override
    public HelpCtx getHelpCtx() {
        return HelpCtx.DEFAULT_HELP;
    }

    @Override
    protected boolean asynchronous() {
        return false;
    }
}

