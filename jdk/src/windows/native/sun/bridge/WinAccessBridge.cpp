/*
 * Copyright (c) 2005, 2014, Oracle and/or its affiliates. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.  Oracle designates this
 * particular file as subject to the "Classpath" exception as provided
 * by Oracle in the LICENSE file that accompanied this code.
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
 * A DLL which is loaded by Windows executables to handle communication
 * between Java VMs purposes of Accessbility.
 */

#include "AccessBridgeDebug.h"
#include "WinAccessBridge.h"
#include "accessBridgeResource.h"
#include "accessBridgeCallbacks.h"
#include "AccessBridgeMessages.h"
#include "AccessBridgeMessageQueue.h"

#include <windows.h>
#include <jni.h>
#include <stdio.h>

// send memory lock
//
// This lock is need to serialize access to the buffer used by sendMemoryPackage.
// If a JVM goes away while the associated memory buffer is in use, a thread switch
// allows a call to JavaVMDestroyed and deallocation of the memory buffer.
CRITICAL_SECTION sendMemoryIPCLock;

// registry paths to newly found JVMs that don't have the bridge installed
char **newJVMs;

WinAccessBridge *theWindowsAccessBridge;
HWND theDialogWindow;

// unique broadcast msg. IDs gotten dymanically
extern UINT theFromJavaHelloMsgID;
extern UINT theFromWindowsHelloMsgID;

// protects the javaVMs chain while in use
bool isVMInstanceChainInUse;

/* =================================================================================== */



/**
 * Proc for "New JVM Found" dialog
 */
BOOL CALLBACK newJVMFoundDialogProc(HWND hwndDlg, UINT message, WPARAM wParam, LPARAM lParam) {

    switch (message) {
    case WM_COMMAND:
        // PrintDebugString("    newJVMDialogProc: LOWORD(wParam) = %d", LOWORD(wParam));

        switch (LOWORD(wParam)) {

            // Remind user later that a new JVM was installed
        case cRemindThereIsNewJVM:
            PrintDebugString("    newJVMDialogProc: cRemindThereIsNewJVM");
            // do nothing
            EndDialog(hwndDlg, wParam);
            return TRUE;

            // Do not remind user later that a new JVM was installed
            /*
        case cDoNotRemindThereIsNewJVM:
            PrintDebugString("    newJVMDialogProc: cDoNotRemindThereIsNewJVM");
            // remember to not remind the user there are new JVMs
            PrintDebugString("theWindowsAccessBridge = %x", theWindowsAccessBridge);
            if (theWindowsAccessBridge != NULL) {
                dontRemindUser(newJVMs);
            }
            EndDialog(hwndDlg, wParam);
            return TRUE;
            */

            // Run the AccessBridge installer
            /*
        case cInstallAccessBridge:
            PrintDebugString("    newJVMDialogProc: cInstallAccessBridge");
            // start the installer
            if (theWindowsAccessBridge != NULL) {
                startInstaller(newJVMs);
            }
            EndDialog(hwndDlg, wParam);
            return TRUE;
            */

        default:
            ;
        }
    default:
        ;
    }
    return FALSE;
}



/* =========================================================================== */

// ---------------------------------------------------------------------------

extern "C" {
    /**
     * DllMain - where Windows executables will load/unload us
     *
     */
    BOOL WINAPI DllMain(HINSTANCE hinstDll, DWORD fdwReason, LPVOID lpvReserved) {

        switch (fdwReason) {
        case DLL_PROCESS_ATTACH:        // A Windows executable loaded us
            PrintDebugString("DLL_PROCESS_ATTACH");
            theWindowsAccessBridge = new WinAccessBridge(hinstDll);
            break;

        case DLL_PROCESS_DETACH:        // A Windows executable unloaded us
            if (theWindowsAccessBridge != (WinAccessBridge *) 0) {
                PrintDebugString("*** AccessBridgeDialogProc -> deleting theWindowsAccessBridge");
                delete theWindowsAccessBridge;
            }
            break;
        }

        return(TRUE);
    }

    /**
     * Append debug info to dialog
     *
     * replaced with code to send output to debug file
     *
     */
    void AppendToCallInfo(char *s) {

        /*
          _CrtDbgReport(_CRT_WARN, (const char *) NULL, NULL, (const char *) NULL,
          (const char *) "WinAccessBridge: %s", s);
        */

        char buf[1024];
        sprintf(buf, "WinAccessBridge: %s", s);
        OutputDebugString(buf);
    }

    /**
     * Our window proc
     *
     */
    BOOL CALLBACK AccessBridgeDialogProc(HWND hDlg, UINT message, UINT wParam, LONG lParam) {
        COPYDATASTRUCT *sentToUs;
        char *package;

        switch (message) {
        case WM_INITDIALOG:
            PrintDebugString("AccessBridgeDialogProc -> Initializing");
            break;

            // call from Java with data for us to deliver
        case WM_COPYDATA:
            if (theDialogWindow == (HWND) wParam) {
                PrintDebugString("AccessBridgeDialogProc -> Got WM_COPYDATA from Java Bridge DLL");
            } else {
                PrintDebugString("AccessBridgeDialogProc -> Got WM_COPYDATA from HWND %p", wParam);
                sentToUs = (COPYDATASTRUCT *) lParam;
                package = (char *) sentToUs->lpData;
                theWindowsAccessBridge->preProcessPackage(package, sentToUs->cbData);
            }
            break;

            // message to ourselves -> de-queue messages and send 'em
        case AB_MESSAGE_QUEUED:
            PrintDebugString("AccessBridgeDialogProc -> Got AB_MESSAGE_QUEUED from ourselves");
            theWindowsAccessBridge->receiveAQueuedPackage();
            break;

            // a JavaAccessBridge DLL is going away
            //
            // When JavaVMDestroyed is called a AccessBridgeJavaVMInstance in the
            // javaVMs chain will be removed.  If that chain is in use this will
            // cause a crash.  One way AB_DLL_GOING_AWAY can arrive is on any
            // outgoing SendMessage call.  SendMessage normally spins waiting for
            // a response.  However, if there is an incoming SendMessage, e.g. for
            // AB_DLL_GOING_AWAY Windows will send that request to this DialogProc.
            // One seemingly easy way to combat that is to use SendMessageTimeout
            // with the SMTO_BLOCK flag set.  However, it has been the case that
            // even after using that technique AB_DLL_GOING_AWAY can still arrive
            // in the middle of processing the javaVMs chain.  An alternative that
            // was tried was to use a critical section around any access ot the
            // javaVMs chain but unfortunately the AB_DLL_GOING_AWAY message arrives
            // on the same thread and thus the use of a critical section is ineffective.
            // The solution then is to set a flag whenever the javaVMs chain is being
            // used and if that flag is set at this point the message will be posted
            // to the message queue.  That would delay the destruction of the instance
            // until the chain is not being traversed.
        case AB_DLL_GOING_AWAY:
            PrintDebugString("***** AccessBridgeDialogProc -> Got AB_DLL_GOING_AWAY message");
            if (isVMInstanceChainInUse) {
                PrintDebugString("  javaVMs chain in use, calling PostMessage");
                PostMessage(hDlg, AB_DLL_GOING_AWAY, wParam, (LPARAM)0);
            } else {
                PrintDebugString("  calling javaVMDestroyed");
                theWindowsAccessBridge->JavaVMDestroyed((HWND) wParam);
            }
            break;

        default:
            // the JavaVM is saying "hi"!
            // wParam == sourceHwnd; lParam == JavaVMID
            if (message == theFromJavaHelloMsgID) {
                PrintDebugString("AccessBridgeDialogProc -> Got theFromJavaHelloMsgID; wParam = %p, lParam = %p", wParam, lParam);
                theWindowsAccessBridge->rendezvousWithNewJavaDLL((HWND) wParam, (long ) lParam);
            }
            break;
        }

        return (FALSE);
    }

}




// ---------------------------------------------------------------------------

/**
 * Initialize the WinAccessBridge
 *
 */
WinAccessBridge::WinAccessBridge(HINSTANCE hInstance) {

    PrintDebugString("WinAccessBridge ctor");

    //  IntializeCriticalSection should only be called once.
    InitializeCriticalSection(&sendMemoryIPCLock);
    windowsInstance = hInstance;
    javaVMs = (AccessBridgeJavaVMInstance *) 0;
    eventHandler = new AccessBridgeEventHandler();
    messageQueue = new AccessBridgeMessageQueue();
    initBroadcastMessageIDs();          // get the unique to us broadcast msg. IDs
    theWindowsAccessBridge = this;
    isVMInstanceChainInUse = false;


    // notify the user if new JVMs are found
    /*
      newJVMs = (char **)malloc(MAX_NEW_JVMS_FOUND);
      for (int i = 0; i < MAX_NEW_JVMS_FOUND; i++) {
      newJVMs[i] = (char *)malloc(SHORT_STRING_SIZE);
      newJVMs[i][0] = 0;
      }

      BOOL newJ2SEFound = findNewJVMs(J2SE_REG_PATH, newJVMs);
      BOOL newJ2REFound = TRUE; // findNewJVMs(J2RE_REG_PATH, newJVMs);

      if (newJ2SEFound || newJ2REFound) {

      int result = DialogBox(windowsInstance,
      "FOUNDNEWJVMDIALOG",
      NULL,
      (DLGPROC)newJVMFoundDialogProc);
      if (result < 0) {
      printError("DialogBox failed");
      }

      PrintDebugString("  FOUNDNEWJVMDIALOG: result = %d", result);

      ShowWindow((HWND)result, SW_SHOW);
      }
    */

    ShowWindow(theDialogWindow, SW_SHOW);
}



/**
 * Destroy the WinAccessBridge
 *
 */
WinAccessBridge::~WinAccessBridge() {
    // inform all other AccessBridges that we're going away
    //  -> shut down all event listening
    //  -> release all objects held in the JVM by us

    PrintDebugString("*****in WinAccessBridge::~WinAccessBridge()");

    // send a broadcast msg.; let other AccessBridge DLLs know we're going away
    AccessBridgeJavaVMInstance *current = javaVMs;
    while (current != (AccessBridgeJavaVMInstance *) 0) {
        PrintDebugString("  telling %p we're going away", current->javaAccessBridgeWindow);
        SendMessage(current->javaAccessBridgeWindow,
                    AB_DLL_GOING_AWAY, (WPARAM) dialogWindow, (LPARAM) 0);
        current = current->nextJVMInstance;
    }

    PrintDebugString("  finished telling JVMs about our demise");

    delete eventHandler;
    delete messageQueue;
    delete javaVMs;

    PrintDebugString("  finished deleting eventHandler, messageQueue, and javaVMs");
    PrintDebugString("GOODBYE CRUEL WORLD...");

    DestroyWindow(theDialogWindow);
}


/**
 * Bring up our window; make a connection to the rest of the world
 *
 */
BOOL
WinAccessBridge::initWindow() {
    theDialogWindow = CreateDialog(windowsInstance,
                                   "ACCESSBRIDGESTATUSWINDOW", NULL,
                                   (DLGPROC) AccessBridgeDialogProc);

    // If window could not be created, return "failure".
    if (!theDialogWindow)
        return (FALSE);

    dialogWindow = theDialogWindow;

    // Make the window visible, update its client area, & return "success".
    // DEBUG_CODE(ShowWindow (theDialogWindow, SW_SHOWNORMAL));
    // DEBUG_CODE(UpdateWindow (theDialogWindow));

    // post a broadcast msg.; let other AccessBridge DLLs know we exist
    PostMessage(HWND_BROADCAST, theFromWindowsHelloMsgID, (WPARAM) dialogWindow, (LPARAM) 0);

    return (TRUE);
}

// -----------------------

/**
 * rendezvousWithNewJavaDLL
 *              - Build AccessBridgeJavaVMInstance data structure
 *                (including setting up Memory-Mapped file info)
 *
 */
LRESULT
WinAccessBridge::rendezvousWithNewJavaDLL(HWND JavaBridgeDLLwindow, long vmID) {
    LRESULT returnVal;

    PrintDebugString("in JavaAccessBridge::rendezvousWithNewJavaDLL(%p, %X)",
                     JavaBridgeDLLwindow, vmID);

    isVMInstanceChainInUse = true;
    AccessBridgeJavaVMInstance *newVM =
        new AccessBridgeJavaVMInstance(dialogWindow, JavaBridgeDLLwindow, vmID, javaVMs);
    javaVMs = newVM;
    isVMInstanceChainInUse = false;

    returnVal = javaVMs->initiateIPC();
    if (returnVal == 0) {

        // tell the newly created JavaVM what events we're interested in, if any
        long javaEventMask = eventHandler->getJavaEventMask();
        long accessibilityEventMask = eventHandler->getAccessibilityEventMask();

        PrintDebugString("  Setting Java event mask to: %X", javaEventMask);

        if (javaEventMask != 0) {
            addJavaEventNotification(javaEventMask);
        }

        PrintDebugString("  Setting Accessibility event mask to: %X", accessibilityEventMask);

        if (accessibilityEventMask != 0) {
            addAccessibilityEventNotification(accessibilityEventMask);
        }
    } else {
        PrintDebugString("  ERROR: Failed to initiate IPC with newly created JavaVM!!!");
        return FALSE;
    }

    PrintDebugString("  Success!!  We rendezvoused with the JavaDLL");
    return returnVal;
}

// -----------------------

/**
 * sendPackage - uses SendMessage(WM_COPYDATA) to do IPC messaging
 *               with the Java AccessBridge DLL
 *
 *               NOTE: WM_COPYDATA is only for one-way IPC; there
 *               is now way to return parameters (especially big ones)
 *               Use sendMemoryPackage() to do that!
 */
void
WinAccessBridge::sendPackage(char *buffer, long bufsize, HWND destWindow) {
    COPYDATASTRUCT toCopy;
    toCopy.dwData = 0;          // 32-bits we could use for something...
    toCopy.cbData = bufsize;
    toCopy.lpData = buffer;

    SendMessage(destWindow, WM_COPYDATA, (WPARAM) dialogWindow, (LPARAM) &toCopy);
}


/**
 * sendMemoryPackage - uses Memory-Mapped files to do IPC messaging
 *                     with the Java AccessBridge DLL, informing the
 *                     Java AccessBridge DLL via SendMessage that something
 *                     is waiting for it in the shared file...
 *
 *                     In the SendMessage call, the third param (WPARAM) is
 *                     the source HWND (theDialogWindow in this case), and
 *                     the fourth param (LPARAM) is the size in bytes of
 *                     the package put into shared memory.
 *
 */
BOOL
WinAccessBridge::sendMemoryPackage(char *buffer, long bufsize, HWND destWindow) {
    if ((AccessBridgeJavaVMInstance *) 0 == javaVMs) {
        return FALSE;
    }
    AccessBridgeJavaVMInstance *ourABJavaVMInstance;
    ourABJavaVMInstance = javaVMs->findABJavaVMInstanceFromJavaHWND(destWindow);
    if (ourABJavaVMInstance != (AccessBridgeJavaVMInstance *) 0) {
        if (!ourABJavaVMInstance->sendMemoryPackage(buffer, bufsize)) {
            // return falue to the caller
            memset(buffer, 0, bufsize);
            return FALSE;
        }
    } else {
        PrintDebugString("ERROR sending memory package: couldn't find destWindow");
        return FALSE;
    }
    return TRUE;
}


/**
 * queuePackage - put a package onto the queue for latter processing
 *
 */
BOOL
WinAccessBridge::queuePackage(char *buffer, long bufsize) {
    PrintDebugString("  in WinAccessBridge::queuePackage(%p, %d)", buffer, bufsize);

    AccessBridgeQueueElement *element = new AccessBridgeQueueElement(buffer, bufsize);

    messageQueue->add(element);
    PostMessage(dialogWindow, AB_MESSAGE_QUEUED, (WPARAM) 0, (LPARAM) 0);
    return TRUE;
}


/**
 * receiveAQueuedPackage - remove a pending packge from the queue and
 *                         handle it. If the queue is busy, post a
 *                         message to self to retrieve it later
 *
 */
BOOL
WinAccessBridge::receiveAQueuedPackage() {
    AccessBridgeQueueElement *element;

    PrintDebugString("in WinAccessBridge::receiveAQueuedPackage()");

    // ensure against re-entrancy problems...
    if (messageQueue->getRemoveLockSetting() == FALSE) {
        messageQueue->setRemoveLock(TRUE);

        PrintDebugString("  dequeueing message");

        QueueReturns result = messageQueue->remove(&element);

        PrintDebugString("   'element->buffer' contains:");
        DEBUG_CODE(PackageType *type = (PackageType *) element->buffer);
        DEBUG_CODE(FocusGainedPackageTag *pkg = (FocusGainedPackageTag *) (((char *) element->buffer) + sizeof(PackageType)));
        DEBUG_CODE(PrintDebugString("     PackageType = %X", *type));
#ifdef ACCESSBRIDGE_ARCH_LEGACY // JOBJECT64 is jobject (32 bit pointer)
        DEBUG_CODE(PrintDebugString("     EventPackage: vmID = %X, event = %p, source = %p", pkg->vmID, pkg->Event, pkg->AccessibleContextSource));
#else // JOBJECT64 is jlong (64 bit)
        DEBUG_CODE(PrintDebugString("     EventPackage: vmID = %X, event = %016I64X, source = %016I64X", pkg->vmID, pkg->Event, pkg->AccessibleContextSource));
#endif
        switch (result) {

        case cQueueBroken:
            PrintDebugString("  ERROR!!! Queue seems to be broken!");
            messageQueue->setRemoveLock(FALSE);
            return FALSE;

        case cMoreMessages:
        case cQueueEmpty:
            if (element != (AccessBridgeQueueElement *) 0) {
                PrintDebugString("  found one; sending it!");
                processPackage(element->buffer, element->bufsize);
                delete element;
            } else {
                PrintDebugString("  ODD... element == 0!");
                return FALSE;
            }
            break;

        case cQueueInUse:
            PrintDebugString("  Queue in use, will try again later...");
            PostMessage(dialogWindow, AB_MESSAGE_QUEUED, (WPARAM) 0, (LPARAM) 0);
            break;

        default:
            messageQueue->setRemoveLock(FALSE);
            return FALSE;       // should never get something we don't recognize!
        }
    } else {
        PrintDebugString("  unable to dequeue message; remove lock is set");
        PostMessage(dialogWindow, AB_MESSAGE_QUEUED, (WPARAM) 0, (LPARAM) 0); // Fix for 6995891
    }

    messageQueue->setRemoveLock(FALSE);
    return TRUE;
}

// -----------------------

/**
 * preProcessPackage
 *              - do triage on incoming packages; queue some, deal with others
 *
 */
void
WinAccessBridge::preProcessPackage(char *buffer, long bufsize) {
    PrintDebugString("PreProcessing package sent from Java:");

    PackageType *type = (PackageType *) buffer;

    switch (*type) {

    PrintDebugString("   type == %X", *type);

    // event packages all get queued for later handling
    //case cPropertyChangePackage:
    case cJavaShutdownPackage:
    case cFocusGainedPackage:
    case cFocusLostPackage:
    case cCaretUpdatePackage:
    case cMouseClickedPackage:
    case cMouseEnteredPackage:
    case cMouseExitedPackage:
    case cMousePressedPackage:
    case cMouseReleasedPackage:
    case cMenuCanceledPackage:
    case cMenuDeselectedPackage:
    case cMenuSelectedPackage:
    case cPopupMenuCanceledPackage:
    case cPopupMenuWillBecomeInvisiblePackage:
    case cPopupMenuWillBecomeVisiblePackage:

    case cPropertyCaretChangePackage:
    case cPropertyDescriptionChangePackage:
    case cPropertyNameChangePackage:
    case cPropertySelectionChangePackage:
    case cPropertyStateChangePackage:
    case cPropertyTextChangePackage:
    case cPropertyValueChangePackage:
    case cPropertyVisibleDataChangePackage:
    case cPropertyChildChangePackage:
    case cPropertyActiveDescendentChangePackage:

    case cPropertyTableModelChangePackage:

        queuePackage(buffer, bufsize);
        break;

        // perhaps there will be some other packages to process at some point... //

    default:
        PrintDebugString("   processing FAILED!! -> don't know how to handle type = %X", *type);
        break;
    }

    PrintDebugString("   package preprocessing completed");
}


#define DISPATCH_EVENT_PACKAGE(packageID, eventPackage, fireEventMethod)            \
    case packageID:                                                                 \
        if (bufsize == sizeof(PackageType) + sizeof(eventPackage)) {                \
            eventPackage *pkg =                                                     \
                (eventPackage *) (buffer + sizeof(PackageType));                    \
            PrintDebugString("   begin callback to AT, type == %X", *type);         \
                theWindowsAccessBridge->eventHandler->fireEventMethod(              \
                    pkg->vmID, pkg->Event, pkg->AccessibleContextSource);           \
                PrintDebugString("   event callback complete!");                    \
        } else {                                                                    \
            PrintDebugString("   processing FAILED!! -> bufsize = %d; expectation = %d", \
                bufsize, sizeof(PackageType) + sizeof(eventPackage));               \
        }                                                                           \
        break;

#define DISPATCH_PROPERTY_CHANGE_PACKAGE(packageID, eventPackage, fireEventMethod, oldValue, newValue) \
    case packageID:                                                                 \
        if (bufsize == sizeof(PackageType) + sizeof(eventPackage)) {                \
            eventPackage *pkg =                                                     \
                (eventPackage *) (buffer + sizeof(PackageType));                    \
            PrintDebugString("   begin callback to AT, type == %X", *type);         \
            theWindowsAccessBridge->eventHandler->fireEventMethod(                  \
                pkg->vmID, pkg->Event, pkg->AccessibleContextSource,                \
                pkg->oldValue, pkg->newValue);                                      \
            PrintDebugString("   event callback complete!");                        \
        } else {                                                                    \
            PrintDebugString("   processing FAILED!! -> bufsize = %d; expectation = %d", \
                bufsize, sizeof(PackageType) + sizeof(eventPackage));               \
        }                                                                           \
        break;

#define DISPATCH_PROPERTY_TABLE_MODEL_CHANGE_PACKAGE(packageID, eventPackage, fireEventMethod, oldValue, newValue) \
    case packageID:                                                                 \
        if (bufsize == sizeof(PackageType) + sizeof(eventPackage)) {                \
            eventPackage *pkg =                                                     \
                (eventPackage *) (buffer + sizeof(PackageType));                    \
            PrintDebugString("   begin callback to AT, type == %X", *type);         \
            theWindowsAccessBridge->eventHandler->fireEventMethod(                  \
                pkg->vmID, pkg->Event, pkg->AccessibleContextSource,                \
                pkg->oldValue, pkg->newValue);                                      \
            PrintDebugString("   event callback complete!");                        \
        } else {                                                                    \
            PrintDebugString("   processing FAILED!! -> bufsize = %d; expectation = %d", \
                bufsize, sizeof(PackageType) + sizeof(eventPackage));                \
        }                                                                            \
        break;

/**
 * processPackage - processes the output of SendMessage(WM_COPYDATA)
 *                  to do IPC messaging with the Java AccessBridge DLL
 *
 */
void
WinAccessBridge::processPackage(char *buffer, long bufsize) {
    PrintDebugString("WinAccessBridge::Processing package sent from Java:");

    PackageType *type = (PackageType *) buffer;

    switch (*type) {

    PrintDebugString("   type == %X", *type);

    case cJavaShutdownPackage:
        PrintDebugString("   type == cJavaShutdownPackage");
        if (bufsize == sizeof(PackageType) + sizeof(JavaShutdownPackage)) {
            JavaShutdownPackage *pkg =
                (JavaShutdownPackage *) (buffer + sizeof(PackageType));
            theWindowsAccessBridge->eventHandler->fireJavaShutdown(pkg->vmID);
            PrintDebugString("   event callback complete!");
            PrintDebugString("   event fired!");
        } else {
            PrintDebugString("   processing FAILED!! -> bufsize = %d; expectation = %d",
                             bufsize, sizeof(PackageType) + sizeof(JavaShutdownPackage));
        }
        break;


        DISPATCH_EVENT_PACKAGE(cFocusGainedPackage, FocusGainedPackage, fireFocusGained);
        DISPATCH_EVENT_PACKAGE(cFocusLostPackage, FocusLostPackage, fireFocusLost);

        DISPATCH_EVENT_PACKAGE(cCaretUpdatePackage, CaretUpdatePackage, fireCaretUpdate);

        DISPATCH_EVENT_PACKAGE(cMouseClickedPackage, MouseClickedPackage, fireMouseClicked);
        DISPATCH_EVENT_PACKAGE(cMouseEnteredPackage, MouseEnteredPackage, fireMouseEntered);
        DISPATCH_EVENT_PACKAGE(cMouseExitedPackage, MouseExitedPackage, fireMouseExited);
        DISPATCH_EVENT_PACKAGE(cMousePressedPackage, MousePressedPackage, fireMousePressed);
        DISPATCH_EVENT_PACKAGE(cMouseReleasedPackage, MouseReleasedPackage, fireMouseReleased);

        DISPATCH_EVENT_PACKAGE(cMenuCanceledPackage, MenuCanceledPackage, fireMenuCanceled);
        DISPATCH_EVENT_PACKAGE(cMenuDeselectedPackage, MenuDeselectedPackage, fireMenuDeselected);
        DISPATCH_EVENT_PACKAGE(cMenuSelectedPackage, MenuSelectedPackage, fireMenuSelected);
        DISPATCH_EVENT_PACKAGE(cPopupMenuCanceledPackage, PopupMenuCanceledPackage, firePopupMenuCanceled);
        DISPATCH_EVENT_PACKAGE(cPopupMenuWillBecomeInvisiblePackage, PopupMenuWillBecomeInvisiblePackage, firePopupMenuWillBecomeInvisible);
        DISPATCH_EVENT_PACKAGE(cPopupMenuWillBecomeVisiblePackage, PopupMenuWillBecomeVisiblePackage, firePopupMenuWillBecomeVisible);

        DISPATCH_PROPERTY_CHANGE_PACKAGE(cPropertyNameChangePackage,
                                         PropertyNameChangePackage,
                                         firePropertyNameChange, oldName, newName)
            DISPATCH_PROPERTY_CHANGE_PACKAGE(cPropertyDescriptionChangePackage,
                                             PropertyDescriptionChangePackage,
                                             firePropertyDescriptionChange,
                                             oldDescription, newDescription)
            DISPATCH_PROPERTY_CHANGE_PACKAGE(cPropertyStateChangePackage,
                                             PropertyStateChangePackage,
                                             firePropertyStateChange, oldState, newState)
            DISPATCH_PROPERTY_CHANGE_PACKAGE(cPropertyValueChangePackage,
                                             PropertyValueChangePackage,
                                             firePropertyValueChange, oldValue, newValue)
            DISPATCH_EVENT_PACKAGE(cPropertySelectionChangePackage,
                                   PropertySelectionChangePackage, firePropertySelectionChange)
            DISPATCH_EVENT_PACKAGE(cPropertyTextChangePackage,
                                   PropertyTextChangePackage, firePropertyTextChange)
            DISPATCH_PROPERTY_CHANGE_PACKAGE(cPropertyCaretChangePackage,
                                             PropertyCaretChangePackage,
                                             firePropertyCaretChange, oldPosition, newPosition)
            DISPATCH_EVENT_PACKAGE(cPropertyVisibleDataChangePackage,
                                   PropertyVisibleDataChangePackage, firePropertyVisibleDataChange)
            DISPATCH_PROPERTY_CHANGE_PACKAGE(cPropertyChildChangePackage,
                                             PropertyChildChangePackage,
                                             firePropertyChildChange,
                                             oldChildAccessibleContext,
                                             newChildAccessibleContext)
            DISPATCH_PROPERTY_CHANGE_PACKAGE(cPropertyActiveDescendentChangePackage,
                                             PropertyActiveDescendentChangePackage,
                                             firePropertyActiveDescendentChange,
                                             oldActiveDescendentAccessibleContext,
                                             newActiveDescendentAccessibleContext)

            DISPATCH_PROPERTY_TABLE_MODEL_CHANGE_PACKAGE(cPropertyTableModelChangePackage,
                                                         PropertyTableModelChangePackage,
                                                         firePropertyTableModelChange,
                                                         oldValue, newValue)


            default:
        PrintDebugString("   processing FAILED!! -> don't know how to handle type = %X", *type);
        break;
    }

    PrintDebugString("   package processing completed");
}


// -----------------------------

void
WinAccessBridge::JavaVMDestroyed(HWND VMBridgeDLLWindow) {
    PrintDebugString("***** WinAccessBridge::JavaVMDestroyed(%p)", VMBridgeDLLWindow);

    if ((AccessBridgeJavaVMInstance *) 0 == javaVMs) {
        return;
    }

    isVMInstanceChainInUse = true;
    AccessBridgeJavaVMInstance *currentVM = javaVMs;
    AccessBridgeJavaVMInstance *previousVM = javaVMs;
    if (javaVMs->javaAccessBridgeWindow == VMBridgeDLLWindow) {
        javaVMs = javaVMs->nextJVMInstance;
        delete currentVM;

        PrintDebugString("  data structures successfully removed");

        // [[[FIXME]]] inform Windows AT that a JVM went away,
        // and that any jobjects it's got lying around for that JVM
        // are now invalid

    } else {
        while (currentVM != (AccessBridgeJavaVMInstance *) 0) {
            if (currentVM->javaAccessBridgeWindow == VMBridgeDLLWindow) {
                previousVM->nextJVMInstance = currentVM->nextJVMInstance;
                delete currentVM;

                PrintDebugString("  data structures successfully removed");

                // [[[FIXME]]] inform Windows AT that a JVM went away,
                // and that any jobjects it's got lying around for that JVM
                // are now invalid
                isVMInstanceChainInUse = false;
                return;
            } else {
                previousVM = currentVM;
                currentVM = currentVM->nextJVMInstance;
            }
        }
        PrintDebugString("  ERROR!! couldn't find matching data structures!");
    }
    isVMInstanceChainInUse = false;
}

// -----------------------

/**
 * releaseJavaObject - lets the JavaVM know it can release the Java Object
 *
 * Note: once you have made this call, the JavaVM will garbage collect
 * the jobject you pass in.  If you later use that jobject in another
 * call, you will cause all maner of havoc!
 *
 */
void
WinAccessBridge::releaseJavaObject(long vmID, JOBJECT64 object) {
#ifdef ACCESSBRIDGE_ARCH_LEGACY // JOBJECT64 is jobject (32 bit pointer)
    PrintDebugString("WinAccessBridge::releaseJavaObject(%X, %p)", vmID, object);
#else // JOBJECT64 is jlong (64 bit)
    PrintDebugString("WinAccessBridge::releaseJavaObject(%X, %016I64X)", vmID, object);
#endif
    if ((AccessBridgeJavaVMInstance *) 0 == javaVMs) {
        return;
    }
    char buffer[sizeof(PackageType) + sizeof(ReleaseJavaObjectPackage)];
    PackageType *type = (PackageType *) buffer;
    ReleaseJavaObjectPackage *pkg = (ReleaseJavaObjectPackage *) (buffer + sizeof(PackageType));
    *type = cReleaseJavaObjectPackage;
    pkg->vmID = vmID;
    pkg->object = object;

    HWND destABWindow = javaVMs->findAccessBridgeWindow(vmID);
    if (destABWindow != (HWND) 0) {
        sendPackage(buffer, sizeof(buffer), destABWindow);              // no return values!
    }
}

// -----------------------

/**
 * getVersionInfo - fill the AccessBridgeVersionInfo struct
 *
 */
BOOL
WinAccessBridge::getVersionInfo(long vmID, AccessBridgeVersionInfo *info) {
    if ((AccessBridgeJavaVMInstance *) 0 == javaVMs) {
        return FALSE;
    }
    char buffer[sizeof(PackageType) + sizeof(GetAccessBridgeVersionPackage)];
    PackageType *type = (PackageType *) buffer;
    GetAccessBridgeVersionPackage *pkg = (GetAccessBridgeVersionPackage *) (buffer + sizeof(PackageType));
    *type = cGetAccessBridgeVersionPackage;
    pkg->vmID = vmID;

    PrintDebugString("WinAccessBridge::getVersionInfo(%X, )", vmID);
    HWND destABWindow = javaVMs->findAccessBridgeWindow(vmID);
    if (destABWindow != (HWND) 0) {
        if (sendMemoryPackage(buffer, sizeof(buffer), destABWindow) == TRUE) {
            memcpy(info, &(pkg->rVersionInfo), sizeof(AccessBridgeVersionInfo));
            PrintDebugString("  VMversion: %ls", info->VMversion);
            PrintDebugString("  bridgeJavaClassVersion: %ls", info->bridgeJavaClassVersion);
            PrintDebugString("  bridgeJavaDLLVersion: %ls", info->bridgeJavaDLLVersion);
            PrintDebugString("  bridgeWinDLLVersion: %ls", info->bridgeWinDLLVersion);
            return TRUE;
        }
    }
    return FALSE;
}


/********** Window-related routines ***********************************/

/**
 * isJavaWindow - returns TRUE if the HWND is a top-level Java Window
 *
 * Note: just because the Windnow is a top-level Java window, that doesn't
 * mean that it is accessible.  Call getAccessibleContextFromHWND(HWND) to get the
 * AccessibleContext, if any, for an HWND that is a Java Window.
 *
 */
BOOL
WinAccessBridge::isJavaWindow(HWND window) {
    HWND hwnd;

    if ((AccessBridgeJavaVMInstance *) 0 == javaVMs) {
        return FALSE;
    }

    // quick check to see if 'window' is top-level; if not, it's not interesting...
    // [[[FIXME]]] is this for sure an OK optimization?
    hwnd = getTopLevelHWND(window);
    if (hwnd == (HWND) NULL) {
        return FALSE;
    }

    PrintDebugString("  in WinAccessBridge::isJavaWindow");



    char buffer[sizeof(PackageType) + sizeof(IsJavaWindowPackage)];
    PackageType *type = (PackageType *) buffer;
    IsJavaWindowPackage *pkg = (IsJavaWindowPackage *) (buffer + sizeof(PackageType));
    *type = cIsJavaWindowPackage;
    pkg->window = (jint) window;

    PrintDebugString("WinAccessBridge::isJavaWindow(%p)", window);

    isVMInstanceChainInUse = true;
    AccessBridgeJavaVMInstance *current = javaVMs;
    while (current != (AccessBridgeJavaVMInstance *) 0) {
        if (sendMemoryPackage(buffer, sizeof(buffer), current->javaAccessBridgeWindow) == TRUE) {
            if (pkg->rResult != 0) {
                isVMInstanceChainInUse = false;
                return TRUE;
            }
        }
        current = current->nextJVMInstance;
    }
    isVMInstanceChainInUse = false;
    return FALSE;


    /*
      char classname[256];
      HWND hwnd;

      hwnd = getTopLevelHWND(window);
      if (hwnd == (HWND) NULL) {
      return FALSE;
      }
      GetClassName(hwnd, classname, 256);

      if (strstr(classname, "AwtFrame") != 0) {
      return TRUE;
      } else if (strstr(classname, "AwtWindow") != 0) {
      return TRUE;
      } else if (strstr(classname, "AwtDialog") != 0) {
      return TRUE;
      }
    */
    // JDK 1.4 introduces new (and changes old) classnames
    /*
      else if (strstr(classname, "SunAwtToolkit") != 0) {
      return TRUE;
      } else if (strstr(classname, "javax.swing.JFrame") != 0) {
      return TRUE;
      }
    */

    return FALSE;
}

/**
 * isSameObject - returns TRUE if the two object references refer to
 *     the same object. Otherwise, this method returns FALSE:
 */
BOOL
WinAccessBridge::isSameObject(long vmID, JOBJECT64 obj1, JOBJECT64 obj2) {

#ifdef ACCESSBRIDGE_ARCH_LEGACY // JOBJECT64 is jobject (32 bit pointer)
    PrintDebugString("WinAccessBridge::isSameObject(%p %p)", obj1, obj2);
#else // JOBJECT64 is jlong (64 bit)
    PrintDebugString("WinAccessBridge::isSameObject(%016I64X %016I64X)", obj1, obj2);
#endif

    if ((AccessBridgeJavaVMInstance *) 0 == javaVMs) {
        return FALSE;
    }

    char buffer[sizeof(PackageType) + sizeof(IsSameObjectPackage)];
    PackageType *type = (PackageType *) buffer;
    IsSameObjectPackage *pkg = (IsSameObjectPackage *) (buffer + sizeof(PackageType));
    *type = cIsSameObjectPackage;
    pkg->vmID = vmID;
    pkg->obj1 = obj1;
    pkg->obj2 = obj2;

    HWND destABWindow = javaVMs->findAccessBridgeWindow(pkg->vmID);
    if (sendMemoryPackage(buffer, sizeof(buffer), destABWindow) == TRUE) {
        if (pkg->rResult != 0) {
            PrintDebugString("  WinAccessBridge::isSameObject returning TRUE (same object)");
            return TRUE;
        } else {
            PrintDebugString("  WinAccessBridge::isSameObject returning FALSE (different object)");
            return FALSE;
        }
    }
    PrintDebugString("  WinAccessBridge::isSameObject returning FALSE (sendMemoryPackage failed)");
    return FALSE;
}

/**
 * FromHWND - returns the AccessibleContext jobject for the HWND
 *
 * Note: this routine can return null, even if the HWND is a Java Window,
 * because the Java Window may not be accessible.
 *
 */
BOOL
WinAccessBridge::getAccessibleContextFromHWND(HWND window, long *vmID, JOBJECT64 *AccessibleContext) {
    if ((AccessBridgeJavaVMInstance *) 0 == javaVMs) {
        return FALSE;
    }

    char buffer[sizeof(PackageType) + sizeof(GetAccessibleContextFromHWNDPackage)];
    PackageType *type = (PackageType *) buffer;
    GetAccessibleContextFromHWNDPackage *pkg = (GetAccessibleContextFromHWNDPackage *) (buffer + sizeof(PackageType));
    *type = cGetAccessibleContextFromHWNDPackage;
    pkg->window = (jint) window;

    PrintDebugString("WinAccessBridge::getAccessibleContextFromHWND(%p, )", window);

    DEBUG_CODE(pkg->rVMID = (long ) 0x01010101);
    DEBUG_CODE(pkg->rAccessibleContext = (JOBJECT64) 0x01010101);

    isVMInstanceChainInUse = true;
    AccessBridgeJavaVMInstance *current = javaVMs;
    while (current != (AccessBridgeJavaVMInstance *) 0) {

        if (sendMemoryPackage(buffer, sizeof(buffer), current->javaAccessBridgeWindow) == TRUE) {
            if (pkg->rAccessibleContext != 0) {
                *vmID = pkg->rVMID;
                *AccessibleContext = (JOBJECT64)pkg->rAccessibleContext;
                PrintDebugString("    current->vmID = %X", current->vmID);
                PrintDebugString("    pkg->rVMID = %X", pkg->rVMID);
#ifdef ACCESSBRIDGE_ARCH_LEGACY // JOBJECT64 is jobject (32 bit pointer)
                PrintDebugString("    pkg->rAccessibleContext = %p", pkg->rAccessibleContext);
#else // JOBJECT64 is jlong (64 bit)
                PrintDebugString("    pkg->rAccessibleContext = %016I64X", pkg->rAccessibleContext);
#endif
                if (pkg->rVMID != current->vmID) {
                    PrintDebugString("    ERROR! getAccessibleContextFromHWND vmIDs don't match!");
                    isVMInstanceChainInUse = false;
                    return FALSE;
                }
                isVMInstanceChainInUse = false;
                return TRUE;
            }
        }
        current = current->nextJVMInstance;
    }
    isVMInstanceChainInUse = false;

    // This isn't really an error; it just means that the HWND was for a non-Java
    // window.  It's also possible the HWND was for a Java window but the JVM has
    // since been shut down and sendMemoryPackage returned FALSE.
    PrintDebugString("    ERROR! getAccessibleContextFromHWND no matching HWND found!");
    return FALSE;
}

/**
 * Returns the HWND for an AccessibleContext.  Returns (HWND)0 on error.
 */
HWND
WinAccessBridge::getHWNDFromAccessibleContext(long vmID, JOBJECT64 accessibleContext) {
    PrintDebugString("  in WinAccessBridge::getHWNDFromAccessibleContext");
    if ((AccessBridgeJavaVMInstance *) 0 == javaVMs) {
        return (HWND)0;
    }

    char buffer[sizeof(PackageType) + sizeof(GetHWNDFromAccessibleContextPackage)];
    PackageType *type = (PackageType *) buffer;
    GetHWNDFromAccessibleContextPackage *pkg = (GetHWNDFromAccessibleContextPackage *) (buffer + sizeof(PackageType));
    *type = cGetHWNDFromAccessibleContextPackage;
    pkg->accessibleContext = accessibleContext;

#ifdef ACCESSBRIDGE_ARCH_LEGACY // JOBJECT64 is jobject (32 bit pointer)
    PrintDebugString("WinAccessBridge::getHWNDFromAccessibleContext(%p)", accessibleContext);
#else // JOBJECT64 is jlong (64 bit)
    PrintDebugString("WinAccessBridge::getHWNDFromAccessibleContext(%016I64X)", accessibleContext);
#endif

    HWND destABWindow = javaVMs->findAccessBridgeWindow(vmID);
    if (destABWindow != (HWND) 0) {
        if (sendMemoryPackage(buffer, sizeof(buffer), destABWindow) == TRUE) {
            return ((HWND)ABLongToHandle(pkg->rHWND));
        }
    }
    return (HWND)0;
}

/********** AccessibleContext routines ***********************************/

/**
 * Walk through Java Windows, in front-to-back Z-order.
 * If NULL is passed it, this function starts at the top.
 *
 */
HWND
WinAccessBridge::getNextJavaWindow(HWND previous) {
    HWND current = previous;
    if (current == NULL) {
        current = GetTopWindow(NULL);
    } else {
        current = GetNextWindow(current, GW_HWNDNEXT);
    }
    while (current != NULL) {
        if (isJavaWindow(current)) {
            return current;
        }
        current = GetNextWindow(current, GW_HWNDNEXT);
    }
    return NULL;
}


/**
 * getAccessibleContextAt - performs the Java code:
 *   Accessible a = EventQueueMonitor.getAccessibleAt(x, y);
 *       return a.getAccessibleContext();
 *
 * Note: this call explicitly goes through the AccessBridge,
 * so that the AccessBridge can hide expected changes in how this functions
 * between JDK 1.1.x w/AccessibilityUtility classes, and JDK 1.2, when some
 * of this functionality may be built into the platform
 *
 */
BOOL
WinAccessBridge::getAccessibleContextAt(long vmID, JOBJECT64 AccessibleContextParent,
                                        jint x, jint y, JOBJECT64 *AccessibleContext) {
    if ((AccessBridgeJavaVMInstance *) 0 == javaVMs) {
        return FALSE;
    }

    char buffer[sizeof(PackageType) + sizeof(GetAccessibleContextAtPackage)];
    PackageType *type = (PackageType *) buffer;
    GetAccessibleContextAtPackage *pkg = (GetAccessibleContextAtPackage *) (buffer + sizeof(PackageType));
    *type = cGetAccessibleContextAtPackage;
    pkg->vmID = vmID;
    pkg->AccessibleContext = AccessibleContextParent;
    pkg->x = x;
    pkg->y = y;

    PrintDebugString("WinAccessBridge::getAccessibleContextAt(%X, %p, %d, %c)", vmID, AccessibleContextParent, x, y);
    HWND destABWindow = javaVMs->findAccessBridgeWindow(pkg->vmID);
    if (sendMemoryPackage(buffer, sizeof(buffer), destABWindow) == TRUE) {
        *AccessibleContext = pkg->rAccessibleContext;
        return TRUE;
    }

    return FALSE;
}


/**
 * getAccessibleContextWithFocus - performs the Java code:
 *   Accessible a = Translator.getAccessible(SwingEventMonitor.getComponentWithFocus());
 *   return a.getAccessibleContext();
 *
 * Note: this call explicitly goes through the AccessBridge,
 * so that the AccessBridge can hide expected changes in how this functions
 * between JDK 1.1.x w/AccessibilityUtility classes, and JDK 1.2, when some
 * of this functionality may be built into the platform
 *
 */
BOOL
WinAccessBridge::getAccessibleContextWithFocus(HWND window, long *vmID, JOBJECT64 *AccessibleContext) {

    if ((AccessBridgeJavaVMInstance *) 0 == javaVMs) {
        return FALSE;
    }
    char buffer[sizeof(PackageType) + sizeof(GetAccessibleContextWithFocusPackage)];
    PackageType *type = (PackageType *) buffer;
    GetAccessibleContextWithFocusPackage *pkg = (GetAccessibleContextWithFocusPackage *) (buffer + sizeof(PackageType));
    *type = cGetAccessibleContextWithFocusPackage;

    PrintDebugString("WinAccessBridge::getAccessibleContextWithFocus(%p, %X, )", window, vmID);
    // find vmID, etc. from HWND; ask that VM for the AC w/Focus
        HWND pkgVMID = (HWND)ABLongToHandle( pkg->rVMID ) ;
    if (getAccessibleContextFromHWND(window, (long *)&(pkgVMID), &(pkg->rAccessibleContext)) == TRUE) {
        HWND destABWindow = javaVMs->findAccessBridgeWindow((long)pkgVMID);     // ineffecient [[[FIXME]]]
        if (sendMemoryPackage(buffer, sizeof(buffer), destABWindow) == TRUE) {
            *vmID = pkg->rVMID;
            *AccessibleContext = pkg->rAccessibleContext;
            return TRUE;
        }
    }

    return FALSE;
}

/**
 * getAccessibleContextInfo - fills a struct with a bunch of information
 * contained in the Java Accessibility API
 *
 *
 * Note: if the AccessibleContext parameter is bogus, this call will blow up
 */
BOOL
WinAccessBridge::getAccessibleContextInfo(long vmID,
                                          JOBJECT64 accessibleContext,
                                          AccessibleContextInfo *info) {
    if ((AccessBridgeJavaVMInstance *) 0 == javaVMs) {
        return FALSE;
    }
    char buffer[sizeof(PackageType) + sizeof(GetAccessibleContextInfoPackage)];
    PackageType *type = (PackageType *) buffer;
    GetAccessibleContextInfoPackage *pkg = (GetAccessibleContextInfoPackage *) (buffer + sizeof(PackageType));
    *type = cGetAccessibleContextInfoPackage;
    pkg->vmID = vmID;
    pkg->AccessibleContext = accessibleContext;

#ifdef ACCESSBRIDGE_ARCH_LEGACY // JOBJECT64 is jobject (32 bit pointer)
    PrintDebugString("WinAccessBridge::getAccessibleContextInfo(%X, %p, )", vmID, accessibleContext);
#else // JOBJECT64 is jlong (64 bit)
    PrintDebugString("WinAccessBridge::getAccessibleContextInfo(%X, %016I64X, )", vmID, accessibleContext);
#endif
    // need to call only the HWND/VM that contains this AC
    HWND destABWindow = javaVMs->findAccessBridgeWindow(vmID);
    if (destABWindow != (HWND) 0) {
        if (sendMemoryPackage(buffer, sizeof(buffer), destABWindow) == TRUE) {
            memcpy(info, &(pkg->rAccessibleContextInfo), sizeof(AccessibleContextInfo));
            PrintDebugString("  name: %ls", info->name);
            PrintDebugString("  description: %ls", info->description);
            PrintDebugString("  role: %ls", info->role);
            PrintDebugString("  role_en_US: %ls", info->role_en_US);
            PrintDebugString("  states: %ls", info->states);
            PrintDebugString("  states_en_US: %ls", info->states_en_US);
            return TRUE;
        }
    }

    return FALSE;
}

/**
 * getAccessibleChildFromContext - performs the Java code:
 *   Accessible child = ac.getAccessibleChild(i);
 *   return child.getAccessibleContext();
 *
 * Note: this call explicitly goes through the AccessBridge,
 * so that the AccessBridge can hide expected changes in how this functions
 * between JDK 1.1.x w/AccessibilityUtility classes, and JDK 1.2, when some
 * of this functionality may be built into the platform
 *
 */
JOBJECT64
WinAccessBridge::getAccessibleChildFromContext(long vmID,
                                               JOBJECT64 AccessibleContext,
                                               jint childIndex) {
    if ((AccessBridgeJavaVMInstance *) 0 == javaVMs) {
        return (JOBJECT64)0;
    }
    char buffer[sizeof(PackageType) + sizeof(GetAccessibleChildFromContextPackage)];
    PackageType *type = (PackageType *) buffer;
    GetAccessibleChildFromContextPackage *pkg = (GetAccessibleChildFromContextPackage *) (buffer + sizeof(PackageType));
    *type = cGetAccessibleChildFromContextPackage;
    pkg->vmID = vmID;
    pkg->AccessibleContext = AccessibleContext;
    pkg->childIndex = childIndex;

#ifdef ACCESSBRIDGE_ARCH_LEGACY // JOBJECT64 is jobject (32 bit pointer)
    PrintDebugString("WinAccessBridge::getAccessibleChildFromContext(%X, %p, %d)", vmID, AccessibleContext, childIndex);
#else // JOBJECT64 is jlong (64 bit)
    PrintDebugString("WinAccessBridge::getAccessibleChildFromContext(%X, %016I64X, %d)", vmID, AccessibleContext, childIndex);
#endif
    // need to call only the HWND/VM that contains this AC
    HWND destABWindow = javaVMs->findAccessBridgeWindow(vmID);
    if (destABWindow != (HWND) 0) {
        if (sendMemoryPackage(buffer, sizeof(buffer), destABWindow) == TRUE) {
            return pkg->rAccessibleContext;
        }
    }

    return (JOBJECT64) 0;
}

/**
 * getAccessibleParentFromContext - returns the parent AccessibleContext jobject
 *
 * Note: this may be null, if the AccessibleContext passed in is a top-level
 * window, then it has no parent.
 *
 */
JOBJECT64
WinAccessBridge::getAccessibleParentFromContext(long vmID,
                                                JOBJECT64 AccessibleContext) {
    if ((AccessBridgeJavaVMInstance *) 0 == javaVMs) {
        return (JOBJECT64)0;
    }
    char buffer[sizeof(PackageType) + sizeof(GetAccessibleParentFromContextPackage)];
    PackageType *type = (PackageType *) buffer;
    GetAccessibleParentFromContextPackage *pkg = (GetAccessibleParentFromContextPackage *) (buffer + sizeof(PackageType));
    *type = cGetAccessibleParentFromContextPackage;
    pkg->vmID = vmID;
    pkg->AccessibleContext = AccessibleContext;

    PrintDebugString("WinAccessBridge::getAccessibleParentFromContext(%X, %p)", vmID, AccessibleContext);
    // need to call only the HWND/VM that contains this AC
    HWND destABWindow = javaVMs->findAccessBridgeWindow(vmID);
    if (destABWindow != (HWND) 0) {
        if (sendMemoryPackage(buffer, sizeof(buffer), destABWindow) == TRUE) {
            return pkg->rAccessibleContext;
        }
    }

    return (JOBJECT64) 0;
}

/********** AccessibleTable routines ***********************************/

BOOL
WinAccessBridge::getAccessibleTableInfo(long vmID,
                                        JOBJECT64 accessibleContext,
                                        AccessibleTableInfo *tableInfo) {

#ifdef ACCESSBRIDGE_ARCH_LEGACY // JOBJECT64 is jobject (32 bit pointer)
    PrintDebugString("##### WinAccessBridge::getAccessibleTableInfo(%X, %p, %p)", vmID, accessibleContext,
                     tableInfo);
#else // JOBJECT64 is jlong (64 bit)
    PrintDebugString("##### WinAccessBridge::getAccessibleTableInfo(%X, %016I64X, %p)", vmID, accessibleContext,
                     tableInfo);
#endif

    if ((AccessBridgeJavaVMInstance *) 0 == javaVMs) {
        return FALSE;
    }
    char buffer[sizeof(PackageType) + sizeof(GetAccessibleTableInfoPackage)];
    PackageType *type = (PackageType *) buffer;
    GetAccessibleTableInfoPackage *pkg = (GetAccessibleTableInfoPackage *) (buffer + sizeof(PackageType));
    *type = cGetAccessibleTableInfoPackage;
    pkg->vmID = vmID;
    pkg->accessibleContext = accessibleContext;

    // need to call only the HWND/VM that contains this AC
    HWND destABWindow = javaVMs->findAccessBridgeWindow(vmID);
    if (destABWindow != (HWND) 0) {
        if (sendMemoryPackage(buffer, sizeof(buffer), destABWindow) == TRUE) {
            memcpy(tableInfo, &(pkg->rTableInfo), sizeof(AccessibleTableInfo));
            if (pkg->rTableInfo.rowCount != -1) {
                PrintDebugString("  ##### WinAccessBridge::getAccessibleTableInfo succeeded");
                return TRUE;
            }
        }
    }
    PrintDebugString("  ##### WinAccessBridge::getAccessibleTableInfo failed");
    return FALSE;
}

BOOL
WinAccessBridge::getAccessibleTableCellInfo(long vmID, JOBJECT64 accessibleTable,
                                            jint row, jint column,
                                            AccessibleTableCellInfo *tableCellInfo) {

    PrintDebugString("##### WinAccessBridge::getAccessibleTableCellInfo(%X, %p, %d, %d, %p)", vmID,
                     accessibleTable, row, column, tableCellInfo);

    if ((AccessBridgeJavaVMInstance *) 0 == javaVMs) {
        return FALSE;
    }

    char buffer[sizeof(PackageType) + sizeof(GetAccessibleTableCellInfoPackage)];
    PackageType *type = (PackageType *) buffer;
    GetAccessibleTableCellInfoPackage *pkg = (GetAccessibleTableCellInfoPackage *) (buffer + sizeof(PackageType));
    *type = cGetAccessibleTableCellInfoPackage;
    pkg->vmID = vmID;
    pkg->accessibleTable = accessibleTable;
    pkg->row = row;
    pkg->column = column;
    HWND destABWindow = javaVMs->findAccessBridgeWindow(vmID);

    if (destABWindow != (HWND) 0) {
        if (sendMemoryPackage(buffer, sizeof(buffer), destABWindow) == TRUE) {
            PrintDebugString("  XXXX pkg->rTableCellInfo.accessibleContext = %p", pkg->rTableCellInfo.accessibleContext);
            memcpy(tableCellInfo, &(pkg->rTableCellInfo), sizeof(AccessibleTableCellInfo));
            PrintDebugString("  ##### WinAccessBridge::getAccessibleTableCellInfo succeeded");
            return TRUE;
        }
    }
    PrintDebugString("  ##### WinAccessBridge::getAccessibleTableCellInfo failed");
    return FALSE;
}


BOOL
WinAccessBridge::getAccessibleTableRowHeader(long vmID, JOBJECT64 accessibleContext, AccessibleTableInfo *tableInfo) {

#ifdef ACCESSBRIDGE_ARCH_LEGACY // JOBJECT64 is jobject (32 bit pointer)
    PrintDebugString("##### WinAccessBridge::getAccessibleTableRowHeader(%X, %p)", vmID, accessibleContext);
#else // JOBJECT64 is jlong (64 bit)
    PrintDebugString("##### WinAccessBridge::getAccessibleTableRowHeader(%X, %016I64X)", vmID, accessibleContext);
#endif

    if ((AccessBridgeJavaVMInstance *) 0 == javaVMs) {
        return FALSE;
    }
    char buffer[sizeof(PackageType) + sizeof(GetAccessibleTableRowHeaderPackage)];
    PackageType *type = (PackageType *) buffer;
    GetAccessibleTableRowHeaderPackage *pkg = (GetAccessibleTableRowHeaderPackage *) (buffer + sizeof(PackageType));
    *type = cGetAccessibleTableRowHeaderPackage;
    pkg->vmID = vmID;
    pkg->accessibleContext = accessibleContext;

    // need to call only the HWND/VM that contains this AC
    HWND destABWindow = javaVMs->findAccessBridgeWindow(vmID);
    if (destABWindow != (HWND) 0) {
        if (sendMemoryPackage(buffer, sizeof(buffer), destABWindow) == TRUE) {
            PrintDebugString("  ##### WinAccessBridge::getAccessibleTableRowHeader succeeded");
            memcpy(tableInfo, &(pkg->rTableInfo), sizeof(AccessibleTableInfo));
            return TRUE;
        }
    }
    PrintDebugString("  ##### WinAccessBridge::getAccessibleTableRowHeader failed");
    return FALSE;
}

BOOL
WinAccessBridge::getAccessibleTableColumnHeader(long vmID, JOBJECT64 accessibleContext, AccessibleTableInfo *tableInfo) {

#ifdef ACCESSBRIDGE_ARCH_LEGACY // JOBJECT64 is jobject (32 bit pointer)
    PrintDebugString("##### WinAccessBridge::getAccessibleTableColumnHeader(%X, %p)", vmID, accessibleContext);
#else // JOBJECT64 is jlong (64 bit)
    PrintDebugString("##### WinAccessBridge::getAccessibleTableColumnHeader(%X, %016I64X)", vmID, accessibleContext);
#endif

    if ((AccessBridgeJavaVMInstance *) 0 == javaVMs) {
        return FALSE;
    }
    char buffer[sizeof(PackageType) + sizeof(GetAccessibleTableColumnHeaderPackage)];
    PackageType *type = (PackageType *) buffer;
    GetAccessibleTableColumnHeaderPackage *pkg = (GetAccessibleTableColumnHeaderPackage *) (buffer + sizeof(PackageType));
    *type = cGetAccessibleTableColumnHeaderPackage;
    pkg->vmID = vmID;
    pkg->accessibleContext = accessibleContext;

    // need to call only the HWND/VM that contains this AC
    HWND destABWindow = javaVMs->findAccessBridgeWindow(vmID);
    if (destABWindow != (HWND) 0) {
        if (sendMemoryPackage(buffer, sizeof(buffer), destABWindow) == TRUE) {
            PrintDebugString("  ##### WinAccessBridge::getAccessibleTableColumnHeader succeeded");
            memcpy(tableInfo, &(pkg->rTableInfo), sizeof(AccessibleTableInfo));
            return TRUE;
        }
    }
    PrintDebugString("  ##### WinAccessBridge::getAccessibleTableColumnHeader failed");
    return FALSE;
}

JOBJECT64
WinAccessBridge::getAccessibleTableRowDescription(long vmID,
                                                  JOBJECT64 accessibleContext,
                                                  jint row) {

#ifdef ACCESSBRIDGE_ARCH_LEGACY // JOBJECT64 is jobject (32 bit pointer)
    PrintDebugString("##### WinAccessBridge::getAccessibleTableRowDescription(%X, %p, %d)", vmID, accessibleContext,
                     row);
#else // JOBJECT64 is jlong (64 bit)
    PrintDebugString("##### WinAccessBridge::getAccessibleTableRowDescription(%X, %016I64X, %d)", vmID, accessibleContext,
                     row);
#endif

    if ((AccessBridgeJavaVMInstance *) 0 == javaVMs) {
        return FALSE;
    }
    char buffer[sizeof(PackageType) + sizeof(GetAccessibleTableRowDescriptionPackage)];
    PackageType *type = (PackageType *) buffer;
    GetAccessibleTableRowDescriptionPackage *pkg = (GetAccessibleTableRowDescriptionPackage *) (buffer + sizeof(PackageType));
    *type = cGetAccessibleTableRowDescriptionPackage;
    pkg->vmID = vmID;
    pkg->row = row;
    pkg->accessibleContext = accessibleContext;

    // need to call only the HWND/VM that contains this AC
    HWND destABWindow = javaVMs->findAccessBridgeWindow(vmID);
    if (destABWindow != (HWND) 0) {
        if (sendMemoryPackage(buffer, sizeof(buffer), destABWindow) == TRUE) {
            PrintDebugString("  ##### WinAccessBridge::getAccessibleTableRowDescription succeeded");
            return pkg->rAccessibleContext;
        }
    }
    PrintDebugString("  ##### WinAccessBridge::getAccessibleTableRowDescription failed");
    return (JOBJECT64)0;
}

JOBJECT64
WinAccessBridge::getAccessibleTableColumnDescription(long vmID,
                                                     JOBJECT64 accessibleContext,
                                                     jint column) {

#ifdef ACCESSBRIDGE_ARCH_LEGACY // JOBJECT64 is jobject (32 bit pointer)
    PrintDebugString("##### WinAccessBridge::getAccessibleTableColumnDescription(%X, %p, %d)", vmID, accessibleContext,
                     column);
#else // JOBJECT64 is jlong (64 bit)
    PrintDebugString("##### WinAccessBridge::getAccessibleTableColumnDescription(%X, %016I64X, %d)", vmID, accessibleContext,
                     column);
#endif

    if ((AccessBridgeJavaVMInstance *) 0 == javaVMs) {
        return FALSE;
    }
    char buffer[sizeof(PackageType) + sizeof(GetAccessibleTableColumnDescriptionPackage)];
    PackageType *type = (PackageType *) buffer;
    GetAccessibleTableColumnDescriptionPackage *pkg =
        (GetAccessibleTableColumnDescriptionPackage *) (buffer + sizeof(PackageType));
    *type = cGetAccessibleTableColumnDescriptionPackage;
    pkg->vmID = vmID;
    pkg->column = column;
    pkg->accessibleContext = accessibleContext;

    // need to call only the HWND/VM that contains this AC
    HWND destABWindow = javaVMs->findAccessBridgeWindow(vmID);
    if (destABWindow != (HWND) 0) {
        if (sendMemoryPackage(buffer, sizeof(buffer), destABWindow) == TRUE) {
            PrintDebugString("  ##### WinAccessBridge::getAccessibleTableColumnDescription succeeded");
            return pkg->rAccessibleContext;
        }
    }
    PrintDebugString("  ##### WinAccessBridge::getAccessibleTableColumnDescription failed");
    return (JOBJECT64)0;
}

jint
WinAccessBridge::getAccessibleTableRowSelectionCount(long vmID, JOBJECT64 accessibleTable) {

#ifdef ACCESSBRIDGE_ARCH_LEGACY // JOBJECT64 is jobject (32 bit pointer)
    PrintDebugString("##### WinAccessBridge::getAccessibleTableRowSelectionCount(%X, %p)", vmID, accessibleTable);
#else // JOBJECT64 is jlong (64 bit)
    PrintDebugString("##### WinAccessBridge::getAccessibleTableRowSelectionCount(%X, %016I64X)", vmID, accessibleTable);
#endif

    if ((AccessBridgeJavaVMInstance *) 0 == javaVMs) {
        return 0;
    }
    char buffer[sizeof(PackageType) + sizeof(GetAccessibleTableRowSelectionCountPackage)];
    PackageType *type = (PackageType *) buffer;
    GetAccessibleTableRowSelectionCountPackage *pkg =
        (GetAccessibleTableRowSelectionCountPackage *) (buffer + sizeof(PackageType));
    *type = cGetAccessibleTableRowSelectionCountPackage;
    pkg->vmID = vmID;
    pkg->accessibleTable = accessibleTable;

    // need to call only the HWND/VM that contains this AC
    HWND destABWindow = javaVMs->findAccessBridgeWindow(vmID);
    if (destABWindow != (HWND) 0) {
        if (sendMemoryPackage(buffer, sizeof(buffer), destABWindow) == TRUE) {
            PrintDebugString("  ##### WinAccessBridge::getAccessibleTableRowSelectionCount succeeded");
            return pkg->rCount;
        }
    }
    PrintDebugString("  ##### WinAccessBridge::getAccessibleTableRowSelectionCount failed");
    return 0;
}

BOOL
WinAccessBridge::isAccessibleTableRowSelected(long vmID, JOBJECT64 accessibleTable, jint row) {

#ifdef ACCESSBRIDGE_ARCH_LEGACY // JOBJECT64 is jobject (32 bit pointer)
    PrintDebugString("##### WinAccessBridge::isAccessibleTableRowSelected(%X, %p)", vmID, accessibleTable);
#else // JOBJECT64 is jlong (64 bit)
    PrintDebugString("##### WinAccessBridge::isAccessibleTableRowSelected(%X, %016I64X)", vmID, accessibleTable);
#endif

    if ((AccessBridgeJavaVMInstance *) 0 == javaVMs) {
        return FALSE;
    }
    char buffer[sizeof(PackageType) + sizeof(IsAccessibleTableRowSelectedPackage)];
    PackageType *type = (PackageType *) buffer;
    IsAccessibleTableRowSelectedPackage *pkg = (IsAccessibleTableRowSelectedPackage *) (buffer + sizeof(PackageType));
    *type = cIsAccessibleTableRowSelectedPackage;
    pkg->vmID = vmID;
    pkg->accessibleTable = accessibleTable;
    pkg->row = row;

    // need to call only the HWND/VM that contains this AC
    HWND destABWindow = javaVMs->findAccessBridgeWindow(vmID);
    if (destABWindow != (HWND) 0) {
        if (sendMemoryPackage(buffer, sizeof(buffer), destABWindow) == TRUE) {
            PrintDebugString("  ##### WinAccessBridge::isAccessibleTableRowSelected succeeded");
            return pkg->rResult;
        }
    }
    PrintDebugString("  ##### WinAccessBridge::isAccessibleTableRowSelected failed");
    return FALSE;
}

BOOL
WinAccessBridge::getAccessibleTableRowSelections(long vmID, JOBJECT64 accessibleTable, jint count, jint *selections) {

#ifdef ACCESSBRIDGE_ARCH_LEGACY // JOBJECT64 is jobject (32 bit pointer)
    PrintDebugString("##### WinAccessBridge::getAccessibleTableRowSelections(%X, %p)", vmID, accessibleTable);
#else // JOBJECT64 is jlong (64 bit)
    PrintDebugString("##### WinAccessBridge::getAccessibleTableRowSelections(%X, %016I64X)", vmID, accessibleTable);
#endif

    if ((AccessBridgeJavaVMInstance *) 0 == javaVMs) {
        return FALSE;
    }
    char buffer[sizeof(PackageType) + sizeof(GetAccessibleTableRowSelectionsPackage)];
    PackageType *type = (PackageType *) buffer;
    GetAccessibleTableRowSelectionsPackage *pkg =
        (GetAccessibleTableRowSelectionsPackage *) (buffer + sizeof(PackageType));
    *type = cGetAccessibleTableRowSelectionsPackage;
    pkg->vmID = vmID;
    pkg->accessibleTable = accessibleTable;
    pkg->count = count;

    // need to call only the HWND/VM that contains this AC
    HWND destABWindow = javaVMs->findAccessBridgeWindow(vmID);
    if (destABWindow != (HWND) 0) {
        if (sendMemoryPackage(buffer, sizeof(buffer), destABWindow) == TRUE) {
            PrintDebugString("  ##### WinAccessBridge::getAccessibleTableRowSelections succeeded");
            memcpy(selections, pkg->rSelections, count * sizeof(jint));
            return TRUE;
        }
    }
    PrintDebugString("  ##### WinAccessBridge::getAccessibleTableRowSelections failed");
    return FALSE;
}


jint
WinAccessBridge::getAccessibleTableColumnSelectionCount(long vmID, JOBJECT64 accessibleTable) {

#ifdef ACCESSBRIDGE_ARCH_LEGACY // JOBJECT64 is jobject (32 bit pointer)
    PrintDebugString("##### WinAccessBridge::getAccessibleTableColumnSelectionCount(%X, %p)", vmID,
                     accessibleTable);
#else // JOBJECT64 is jlong (64 bit)
    PrintDebugString("##### WinAccessBridge::getAccessibleTableColumnSelectionCount(%X, %016I64X)", vmID,
                     accessibleTable);
#endif

    if ((AccessBridgeJavaVMInstance *) 0 == javaVMs) {
        return FALSE;
    }
    char buffer[sizeof(PackageType) + sizeof(GetAccessibleTableColumnSelectionCountPackage)];
    PackageType *type = (PackageType *) buffer;
    GetAccessibleTableColumnSelectionCountPackage *pkg =
        (GetAccessibleTableColumnSelectionCountPackage *) (buffer + sizeof(PackageType));
    *type = cGetAccessibleTableColumnSelectionCountPackage;
    pkg->vmID = vmID;
    pkg->accessibleTable = accessibleTable;

    // need to call only the HWND/VM that contains this AC
    HWND destABWindow = javaVMs->findAccessBridgeWindow(vmID);
    if (destABWindow != (HWND) 0) {
        if (sendMemoryPackage(buffer, sizeof(buffer), destABWindow) == TRUE) {
            PrintDebugString("  ##### WinAccessBridge::getAccessibleTableColumnSelectionCount succeeded");
            return pkg->rCount;
        }
    }
    PrintDebugString("  ##### WinAccessBridge::getAccessibleTableColumnSelectionCount failed");
    return 0;
}

BOOL
WinAccessBridge::isAccessibleTableColumnSelected(long vmID, JOBJECT64 accessibleTable, jint column) {
#ifdef ACCESSBRIDGE_ARCH_LEGACY // JOBJECT64 is jobject (32 bit pointer)
    PrintDebugString("##### WinAccessBridge::isAccessibleTableColumnSelected(%X, %p)", vmID, accessibleTable);
#else // JOBJECT64 is jlong (64 bit)
    PrintDebugString("##### WinAccessBridge::isAccessibleTableColumnSelected(%X, %016I64X)", vmID, accessibleTable);
#endif

    if ((AccessBridgeJavaVMInstance *) 0 == javaVMs) {
        return FALSE;
    }
    char buffer[sizeof(PackageType) + sizeof(IsAccessibleTableColumnSelectedPackage)];
    PackageType *type = (PackageType *) buffer;
    IsAccessibleTableColumnSelectedPackage *pkg = (IsAccessibleTableColumnSelectedPackage *) (buffer + sizeof(PackageType));
    *type = cIsAccessibleTableColumnSelectedPackage;
    pkg->vmID = vmID;
    pkg->accessibleTable = accessibleTable;
    pkg->column = column;

    // need to call only the HWND/VM that contains this AC
    HWND destABWindow = javaVMs->findAccessBridgeWindow(vmID);
    if (destABWindow != (HWND) 0) {
        if (sendMemoryPackage(buffer, sizeof(buffer), destABWindow) == TRUE) {
            PrintDebugString("  ##### WinAccessBridge::isAccessibleTableColumnSelected succeeded");
            return pkg->rResult;
        }
    }
    PrintDebugString("  ##### WinAccessBridge::isAccessibleTableColumnSelected failed");
    return FALSE;
}

BOOL
WinAccessBridge::getAccessibleTableColumnSelections(long vmID, JOBJECT64 accessibleTable, jint count,
                                                    jint *selections) {

#ifdef ACCESSBRIDGE_ARCH_LEGACY // JOBJECT64 is jobject (32 bit pointer)
    PrintDebugString("##### WinAccessBridge::getAccessibleTableColumnSelections(%X, %p)", vmID, accessibleTable);
#else // JOBJECT64 is jlong (64 bit)
    PrintDebugString("##### WinAccessBridge::getAccessibleTableColumnSelections(%X, %016I64X)", vmID, accessibleTable);
#endif

    if ((AccessBridgeJavaVMInstance *) 0 == javaVMs) {
        return FALSE;
    }
    char buffer[sizeof(PackageType) + sizeof(GetAccessibleTableColumnSelectionsPackage)];
    PackageType *type = (PackageType *) buffer;
    GetAccessibleTableColumnSelectionsPackage *pkg =
        (GetAccessibleTableColumnSelectionsPackage *) (buffer + sizeof(PackageType));
    *type = cGetAccessibleTableColumnSelectionsPackage;
    pkg->vmID = vmID;
    pkg->count = count;
    pkg->accessibleTable = accessibleTable;

    // need to call only the HWND/VM that contains this AC
    HWND destABWindow = javaVMs->findAccessBridgeWindow(vmID);
    if (destABWindow != (HWND) 0) {
        if (sendMemoryPackage(buffer, sizeof(buffer), destABWindow) == TRUE) {
            PrintDebugString("  ##### WinAccessBridge::getAccessibleTableColumnSelections succeeded");
            memcpy(selections, pkg->rSelections, count * sizeof(jint));
            return TRUE;
        }
    }
    PrintDebugString("  ##### WinAccessBridge::getAccessibleTableColumnSelections failed");
    return FALSE;
}

jint
WinAccessBridge::getAccessibleTableRow(long vmID, JOBJECT64 accessibleTable, jint index) {

#ifdef ACCESSBRIDGE_ARCH_LEGACY // JOBJECT64 is jobject (32 bit pointer)
    PrintDebugString("##### WinAccessBridge::getAccessibleTableRow(%X, %p, index=%d)", vmID,
                     accessibleTable, index);
#else // JOBJECT64 is jlong (64 bit)
    PrintDebugString("##### WinAccessBridge::getAccessibleTableRow(%X, %016I64X, index=%d)", vmID,
                     accessibleTable, index);
#endif

    if ((AccessBridgeJavaVMInstance *) 0 == javaVMs) {
        return FALSE;
    }
    char buffer[sizeof(PackageType) + sizeof(GetAccessibleTableRowPackage)];
    PackageType *type = (PackageType *) buffer;
    GetAccessibleTableRowPackage *pkg =
        (GetAccessibleTableRowPackage *) (buffer + sizeof(PackageType));
    *type = cGetAccessibleTableRowPackage;
    pkg->vmID = vmID;
    pkg->accessibleTable = accessibleTable;
    pkg->index = index;

    // need to call only the HWND/VM that contains this AC
    HWND destABWindow = javaVMs->findAccessBridgeWindow(vmID);
    if (destABWindow != (HWND) 0) {
        if (sendMemoryPackage(buffer, sizeof(buffer), destABWindow) == TRUE) {
            PrintDebugString("  ##### WinAccessBridge::getAccessibleTableRow succeeded");
            return pkg->rRow;
        }
    }
    PrintDebugString("  ##### WinAccessBridge::getAccessibleTableRow failed");
    return 0;
}

jint
WinAccessBridge::getAccessibleTableColumn(long vmID, JOBJECT64 accessibleTable, jint index) {

#ifdef ACCESSBRIDGE_ARCH_LEGACY // JOBJECT64 is jobject (32 bit pointer)
    PrintDebugString("##### WinAccessBridge::getAccessibleTableColumn(%X, %p, index=%d)", vmID,
                     accessibleTable, index);
#else // JOBJECT64 is jlong (64 bit)
    PrintDebugString("##### WinAccessBridge::getAccessibleTableColumn(%X, %016I64X, index=%d)", vmID,
                     accessibleTable, index);
#endif

    if ((AccessBridgeJavaVMInstance *) 0 == javaVMs) {
        return FALSE;
    }
    char buffer[sizeof(PackageType) + sizeof(GetAccessibleTableColumnPackage)];
    PackageType *type = (PackageType *) buffer;
    GetAccessibleTableColumnPackage *pkg =
        (GetAccessibleTableColumnPackage *) (buffer + sizeof(PackageType));
    *type = cGetAccessibleTableColumnPackage;
    pkg->vmID = vmID;
    pkg->accessibleTable = accessibleTable;
    pkg->index = index;

    // need to call only the HWND/VM that contains this AC
    HWND destABWindow = javaVMs->findAccessBridgeWindow(vmID);
    if (destABWindow != (HWND) 0) {
        if (sendMemoryPackage(buffer, sizeof(buffer), destABWindow) == TRUE) {
            PrintDebugString("  ##### WinAccessBridge::getAccessibleTableColumn succeeded");
            return pkg->rColumn;
        }
    }
    PrintDebugString("  ##### WinAccessBridge::getAccessibleTableColumn failed");
    return 0;
}

jint
WinAccessBridge::getAccessibleTableIndex(long vmID, JOBJECT64 accessibleTable, jint row, jint column) {

#ifdef ACCESSBRIDGE_ARCH_LEGACY // JOBJECT64 is jobject (32 bit pointer)
    PrintDebugString("##### WinAccessBridge::getAccessibleTableIndex(%X, %p, row=%d, col=%d)", vmID,
                     accessibleTable, row, column);
#else // JOBJECT64 is jlong (64 bit)
    PrintDebugString("##### WinAccessBridge::getAccessibleTableIndex(%X, %016I64X, row=%d, col=%d)", vmID,
                     accessibleTable, row, column);
#endif

    if ((AccessBridgeJavaVMInstance *) 0 == javaVMs) {
        return FALSE;
    }
    char buffer[sizeof(PackageType) + sizeof(GetAccessibleTableIndexPackage)];
    PackageType *type = (PackageType *) buffer;
    GetAccessibleTableIndexPackage *pkg =
        (GetAccessibleTableIndexPackage *) (buffer + sizeof(PackageType));
    *type = cGetAccessibleTableIndexPackage;
    pkg->vmID = vmID;
    pkg->accessibleTable = accessibleTable;
    pkg->row = row;
    pkg->column = column;

    // need to call only the HWND/VM that contains this AC
    HWND destABWindow = javaVMs->findAccessBridgeWindow(vmID);
    if (destABWindow != (HWND) 0) {
        if (sendMemoryPackage(buffer, sizeof(buffer), destABWindow) == TRUE) {
            PrintDebugString("  ##### WinAccessBridge::getAccessibleTableIndex succeeded");
            return pkg->rIndex;
        }
    }
    PrintDebugString("  ##### WinAccessBridge::getAccessibleTableIndex failed");
    return 0;
}

/********** end AccessibleTable routines ******************************/

BOOL
WinAccessBridge::getAccessibleRelationSet(long vmID, JOBJECT64 accessibleContext,
                                          AccessibleRelationSetInfo *relationSetInfo) {

#ifdef ACCESSBRIDGE_ARCH_LEGACY // JOBJECT64 is jobject (32 bit pointer)
    PrintDebugString("##### WinAccessBridge::getAccessibleRelationSet(%X, %p, %X)", vmID,
                     accessibleContext, relationSetInfo);
#else // JOBJECT64 is jlong (64 bit)
    PrintDebugString("##### WinAccessBridge::getAccessibleRelationSet(%X, %016I64X, %X)", vmID,
                     accessibleContext, relationSetInfo);
#endif

    if ((AccessBridgeJavaVMInstance *) 0 == javaVMs) {
        return FALSE;
    }

    char buffer[sizeof(PackageType) + sizeof(GetAccessibleRelationSetPackage)];
    PackageType *type = (PackageType *) buffer;
    GetAccessibleRelationSetPackage *pkg = (GetAccessibleRelationSetPackage *) (buffer + sizeof(PackageType));
    *type = cGetAccessibleRelationSetPackage;
    pkg->vmID = vmID;
    pkg->accessibleContext = accessibleContext;

    HWND destABWindow = javaVMs->findAccessBridgeWindow(vmID);
    if (destABWindow != (HWND) 0) {
        if (sendMemoryPackage(buffer, sizeof(buffer), destABWindow) == TRUE) {
            PrintDebugString("  ##### pkg->rAccessibleRelationSetInfo.relationCount = %X",
                             pkg->rAccessibleRelationSetInfo.relationCount);
            memcpy(relationSetInfo, &(pkg->rAccessibleRelationSetInfo), sizeof(AccessibleRelationSetInfo));
            PrintDebugString("  ##### WinAccessBridge::getAccessibleRelationSet succeeded");
            return TRUE;
        }
    }
    PrintDebugString("  ##### WinAccessBridge::getAccessibleRelationSet failed");
    return FALSE;
}


/********** AccessibleHypertext routines ***********/

BOOL
WinAccessBridge::getAccessibleHypertext(long vmID, JOBJECT64 accessibleContext,
                                        AccessibleHypertextInfo *hypertextInfo) {

#ifdef ACCESSBRIDGE_ARCH_LEGACY // JOBJECT64 is jobject (32 bit pointer)
    PrintDebugString("##### WinAccessBridge::getAccessibleHypertext(%X, %p, %X)", vmID,
                     accessibleContext, hypertextInfo);
#else // JOBJECT64 is jlong (64 bit)
    PrintDebugString("##### WinAccessBridge::getAccessibleHypertext(%X, %016I64X, %X)", vmID,
                     accessibleContext, hypertextInfo);
#endif

    if ((AccessBridgeJavaVMInstance *) 0 == javaVMs) {
        return FALSE;
    }

    char buffer[sizeof(PackageType) + sizeof(GetAccessibleHypertextPackage)];
    PackageType *type = (PackageType *) buffer;
    GetAccessibleHypertextPackage *pkg = (GetAccessibleHypertextPackage *) (buffer + sizeof(PackageType));
    *type = cGetAccessibleHypertextPackage;
    pkg->vmID = vmID;
    pkg->accessibleContext = accessibleContext;

    HWND destABWindow = javaVMs->findAccessBridgeWindow(vmID);
    if (destABWindow != (HWND) 0) {
        if (sendMemoryPackage(buffer, sizeof(buffer), destABWindow) == TRUE) {
            memcpy(hypertextInfo, &(pkg->rAccessibleHypertextInfo), sizeof(AccessibleHypertextInfo));

            PrintDebugString("  ##### hypertextInfo.linkCount = %d", hypertextInfo->linkCount);
            PrintDebugString("  ##### WinAccessBridge::getAccessibleHypertext succeeded");

            return TRUE;
        }
    }
    PrintDebugString("  ##### WinAccessBridge::getAccessibleHypertext failed");
    return FALSE;
}


BOOL
WinAccessBridge::activateAccessibleHyperlink(long vmID, JOBJECT64 accessibleContext,
                                             JOBJECT64 accessibleHyperlink) {

#ifdef ACCESSBRIDGE_ARCH_LEGACY // JOBJECT64 is jobject (32 bit pointer)
    PrintDebugString("WinAccessBridge::activateAccessibleHyperlink(%p %p)", accessibleContext,
                     accessibleHyperlink);
#else // JOBJECT64 is jlong (64 bit)
    PrintDebugString("WinAccessBridge::activateAccessibleHyperlink(%016I64X %016I64X)", accessibleContext,
                     accessibleHyperlink);
#endif

    if ((AccessBridgeJavaVMInstance *) 0 == javaVMs) {
        return FALSE;
    }

    char buffer[sizeof(PackageType) + sizeof(ActivateAccessibleHyperlinkPackage)];
    PackageType *type = (PackageType *) buffer;
    ActivateAccessibleHyperlinkPackage *pkg = (ActivateAccessibleHyperlinkPackage *) (buffer + sizeof(PackageType));
    *type = cActivateAccessibleHyperlinkPackage;
    pkg->vmID = vmID;
    pkg->accessibleContext = accessibleContext;
    pkg->accessibleHyperlink = accessibleHyperlink;

    HWND destABWindow = javaVMs->findAccessBridgeWindow(pkg->vmID);
    if (sendMemoryPackage(buffer, sizeof(buffer), destABWindow) == TRUE) {
        return pkg->rResult;
    }
    PrintDebugString("  WinAccessBridge::activateAccessibleHyperlink returning FALSE (sendMemoryPackage failed)");
    return FALSE;
}

/*
 * Returns the number of hyperlinks in a component
 * Maps to AccessibleHypertext.getLinkCount.
 * Returns -1 on error.
 */
jint
WinAccessBridge::getAccessibleHyperlinkCount(const long vmID,
                                             const AccessibleContext accessibleContext) {

#ifdef ACCESSBRIDGE_ARCH_LEGACY // JOBJECT64 is jobject (32 bit pointer)
    PrintDebugString("##### WinAccessBridge::getAccessibleHyperlinkCount(%X, %p)",
                     vmID, accessibleContext);
#else // JOBJECT64 is jlong (64 bit)
    PrintDebugString("##### WinAccessBridge::getAccessibleHyperlinkCount(%X, %016I64X)",
                     vmID, accessibleContext);
#endif

    if ((AccessBridgeJavaVMInstance *) 0 == javaVMs) {
        return FALSE;
    }

    char buffer[sizeof(PackageType) + sizeof(GetAccessibleHyperlinkCountPackage)];
    PackageType *type = (PackageType *) buffer;
    GetAccessibleHyperlinkCountPackage *pkg = (GetAccessibleHyperlinkCountPackage *) (buffer + sizeof(PackageType));
    *type = cGetAccessibleHyperlinkCountPackage;
    pkg->vmID = vmID;
    pkg->accessibleContext = accessibleContext;

    HWND destABWindow = javaVMs->findAccessBridgeWindow(vmID);
    if (destABWindow != (HWND) 0) {
        if (sendMemoryPackage(buffer, sizeof(buffer), destABWindow) == TRUE) {
            PrintDebugString("  ##### hypetext link count = %d", pkg->rLinkCount);
            PrintDebugString("  ##### WinAccessBridge::getAccessibleHyperlinkCount succeeded");
            return pkg->rLinkCount;
        }
    }
    PrintDebugString("  ##### WinAccessBridge::getAccessibleHyperlinkCount failed");
    return -1;
}

/*
 * This method is used to iterate through the hyperlinks in a component.  It
 * returns hypertext information for a component starting at hyperlink index
 * nStartIndex.  No more than MAX_HYPERLINKS AccessibleHypertextInfo objects will
 * be returned for each call to this method.
 * returns FALSE on error.
 */
BOOL
WinAccessBridge::getAccessibleHypertextExt(const long vmID,
                                           const AccessibleContext accessibleContext,
                                           const jint startIndex,
                                           /* OUT */ AccessibleHypertextInfo *hypertextInfo) {

#ifdef ACCESSBRIDGE_ARCH_LEGACY // JOBJECT64 is jobject (32 bit pointer)
    PrintDebugString("##### WinAccessBridge::getAccessibleHypertextExt(%X, %p %p)", vmID,
                     accessibleContext, hypertextInfo);
#else // JOBJECT64 is jlong (64 bit)
    PrintDebugString("##### WinAccessBridge::getAccessibleHypertextExt(%X, %016I64X %p)", vmID,
                     accessibleContext, hypertextInfo);
#endif

    if ((AccessBridgeJavaVMInstance *) 0 == javaVMs) {
        return FALSE;
    }

    char buffer[sizeof(PackageType) + sizeof(GetAccessibleHypertextExtPackage)];
    PackageType *type = (PackageType *) buffer;
    GetAccessibleHypertextExtPackage *pkg = (GetAccessibleHypertextExtPackage *) (buffer + sizeof(PackageType));
    *type = cGetAccessibleHypertextExtPackage;
    pkg->vmID = vmID;
    pkg->accessibleContext = accessibleContext;
    pkg->startIndex = startIndex;

    HWND destABWindow = javaVMs->findAccessBridgeWindow(vmID);
    if (destABWindow != (HWND) 0) {
        if (sendMemoryPackage(buffer, sizeof(buffer), destABWindow) == TRUE) {
            PrintDebugString("  ##### pkg->rSuccess = %d", pkg->rSuccess);

            memcpy(hypertextInfo, &(pkg->rAccessibleHypertextInfo), sizeof(AccessibleHypertextInfo));
            if (pkg->rSuccess == TRUE) {
                PrintDebugString("  ##### hypertextInfo.linkCount = %d", hypertextInfo->linkCount);
                PrintDebugString("  ##### hypertextInfo.linkCount = %d", hypertextInfo->linkCount);
            } else {
                PrintDebugString("  ##### WinAccessBridge::getAccessibleHypertextExt failed");
            }
            return pkg->rSuccess;;
        }
    }
    PrintDebugString("  ##### WinAccessBridge::getAccessibleHypertextExt failed");
    return FALSE;
}


/*
 * Returns the index into an array of hyperlinks that is associated with
 * a character index in document;
 * Maps to AccessibleHypertext.getLinkIndex.
 * Returns -1 on error.
 */
jint
WinAccessBridge::getAccessibleHypertextLinkIndex(const long vmID,
                                                 const AccessibleHyperlink hypertext,
                                                 const jint charIndex) {

#ifdef ACCESSBRIDGE_ARCH_LEGACY // JOBJECT64 is jobject (32 bit pointer)
    PrintDebugString("##### WinAccessBridge::getAccessibleHypertextLinkIndex(%X, %p)",
                     vmID, hypertext);
#else // JOBJECT64 is jlong (64 bit)
    PrintDebugString("##### WinAccessBridge::getAccessibleHypertextLinkIndex(%X, %016I64X)",
                     vmID, hypertext);
#endif

    if ((AccessBridgeJavaVMInstance *) 0 == javaVMs) {
        return FALSE;
    }

    char buffer[sizeof(PackageType) + sizeof(GetAccessibleHypertextLinkIndexPackage)];
    PackageType *type = (PackageType *) buffer;
    GetAccessibleHypertextLinkIndexPackage *pkg = (GetAccessibleHypertextLinkIndexPackage *) (buffer + sizeof(PackageType));
    *type = cGetAccessibleHypertextLinkIndexPackage;
    pkg->vmID = vmID;
    pkg->hypertext = hypertext;
    pkg->charIndex = charIndex;

    HWND destABWindow = javaVMs->findAccessBridgeWindow(vmID);
    if (destABWindow != (HWND) 0) {
        if (sendMemoryPackage(buffer, sizeof(buffer), destABWindow) == TRUE) {
            PrintDebugString("  ##### hypetext link index = %d", pkg->rLinkIndex);
            PrintDebugString("  ##### WinAccessBridge::getAccessibleHypertextLinkIndex  succeeded");
            return pkg->rLinkIndex;
        }
    }
    PrintDebugString("  ##### WinAccessBridge::getAccessibleHypertextLinkIndex  failed");
    return -1;
}

/*
 * Returns the nth hyperlink in a document.
 * Maps to AccessibleHypertext.getLink.
 * Returns -1 on error
 */
BOOL
WinAccessBridge::getAccessibleHyperlink(const long vmID,
                                        const AccessibleHyperlink hypertext,
                                        const jint linkIndex,
                                        /* OUT */ AccessibleHyperlinkInfo *hyperlinkInfo) {

#ifdef ACCESSBRIDGE_ARCH_LEGACY // JOBJECT64 is jobject (32 bit pointer)
    PrintDebugString("##### WinAccessBridge::getAccessibleHyperlink(%X, %p, %p)", vmID,
                     hypertext, hyperlinkInfo);
#else // JOBJECT64 is jlong (64 bit)
    PrintDebugString("##### WinAccessBridge::getAccessibleHyperlink(%X, %016I64X, %p)", vmID,
                     hypertext, hyperlinkInfo);
#endif

    if ((AccessBridgeJavaVMInstance *) 0 == javaVMs) {
        return FALSE;
    }

    char buffer[sizeof(PackageType) + sizeof(GetAccessibleHyperlinkPackage)];
    PackageType *type = (PackageType *) buffer;
    GetAccessibleHyperlinkPackage *pkg = (GetAccessibleHyperlinkPackage *) (buffer + sizeof(PackageType));
    *type = cGetAccessibleHyperlinkPackage;
    pkg->vmID = vmID;
    pkg->hypertext = hypertext;
    pkg->linkIndex = linkIndex;

    HWND destABWindow = javaVMs->findAccessBridgeWindow(vmID);
    if (destABWindow != (HWND) 0) {
        if (sendMemoryPackage(buffer, sizeof(buffer), destABWindow) == TRUE) {
            memcpy(hyperlinkInfo, &(pkg->rAccessibleHyperlinkInfo),
                   sizeof(AccessibleHyperlinkInfo));
            PrintDebugString("  ##### WinAccessBridge::getAccessibleHypertext succeeded");
            return TRUE;
        }
    }
    PrintDebugString("  ##### WinAccessBridge::getAccessibleHypertext failed");
    return FALSE;
}


/********** AccessibleKeyBinding routines ***********/

BOOL
WinAccessBridge::getAccessibleKeyBindings(long vmID, JOBJECT64 accessibleContext,
                                          AccessibleKeyBindings *keyBindings) {

#ifdef ACCESSBRIDGE_ARCH_LEGACY // JOBJECT64 is jobject (32 bit pointer)
    PrintDebugString("##### WinAccessBridge::getAccessibleKeyBindings(%X, %p, %p)", vmID,
                     accessibleContext, keyBindings);
#else // JOBJECT64 is jlong (64 bit)
    PrintDebugString("##### WinAccessBridge::getAccessibleKeyBindings(%X, %016I64X, %p)", vmID,
                     accessibleContext, keyBindings);
#endif

    if ((AccessBridgeJavaVMInstance *) 0 == javaVMs) {
        return FALSE;
    }

    char buffer[sizeof(PackageType) + sizeof(GetAccessibleKeyBindingsPackage)];
    PackageType *type = (PackageType *) buffer;
    GetAccessibleKeyBindingsPackage *pkg = (GetAccessibleKeyBindingsPackage *) (buffer + sizeof(PackageType));
    *type = cGetAccessibleKeyBindingsPackage;
    pkg->vmID = vmID;
    pkg->accessibleContext = accessibleContext;

    HWND destABWindow = javaVMs->findAccessBridgeWindow(vmID);
    if (destABWindow != (HWND) 0) {
        if (sendMemoryPackage(buffer, sizeof(buffer), destABWindow) == TRUE) {
            memcpy(keyBindings, &(pkg->rAccessibleKeyBindings), sizeof(AccessibleKeyBindings));

            PrintDebugString("  ##### keyBindings.keyBindingsCount = %d", keyBindings->keyBindingsCount);
            for (int i = 0; i < keyBindings->keyBindingsCount; ++i) {
                PrintDebugString("  Key Binding # %d", i+1);
                PrintDebugString("    Modifiers: 0x%x", keyBindings->keyBindingInfo[i].modifiers);
                PrintDebugString("    Character (hex):  0x%x", keyBindings->keyBindingInfo[i].character);
                PrintDebugString("    Character (wide char):  %lc", keyBindings->keyBindingInfo[i].character);
            }
            PrintDebugString("  ##### WinAccessBridge::getAccessibleKeyBindings succeeded");

            return TRUE;
        }
    }
    PrintDebugString("  ##### WinAccessBridge::getAccessibleKeyBindings failed");
    return FALSE;
}

BOOL
WinAccessBridge::getAccessibleIcons(long vmID, JOBJECT64 accessibleContext, AccessibleIcons *icons) {

#ifdef ACCESSBRIDGE_ARCH_LEGACY // JOBJECT64 is jobject (32 bit pointer)
    PrintDebugString("##### WinAccessBridge::getAccessibleIcons(%X, %p, %p)", vmID,
                     accessibleContext, icons);
#else // JOBJECT64 is jlong (64 bit)
    PrintDebugString("##### WinAccessBridge::getAccessibleIcons(%X, %016I64X, %p)", vmID,
                     accessibleContext, icons);
#endif

    if ((AccessBridgeJavaVMInstance *) 0 == javaVMs) {
        return FALSE;
    }

    char buffer[sizeof(PackageType) + sizeof(GetAccessibleIconsPackage)];
    PackageType *type = (PackageType *) buffer;
    GetAccessibleIconsPackage *pkg = (GetAccessibleIconsPackage *) (buffer + sizeof(PackageType));
    *type = cGetAccessibleIconsPackage;
    pkg->vmID = vmID;
    pkg->accessibleContext = accessibleContext;

    HWND destABWindow = javaVMs->findAccessBridgeWindow(vmID);
    if (destABWindow != (HWND) 0) {
        if (sendMemoryPackage(buffer, sizeof(buffer), destABWindow) == TRUE) {
            memcpy(icons, &(pkg->rAccessibleIcons), sizeof(AccessibleIcons));

            PrintDebugString("  ##### icons.iconsCount = %d", icons->iconsCount);
            PrintDebugString("  ##### WinAccessBridge::getAccessibleIcons succeeded");

            return TRUE;
        }
    }
    PrintDebugString("  ##### WinAccessBridge::getAccessibleIcons failed");
    return FALSE;
}

BOOL
WinAccessBridge::getAccessibleActions(long vmID, JOBJECT64 accessibleContext, AccessibleActions *actions) {

#ifdef ACCESSBRIDGE_ARCH_LEGACY // JOBJECT64 is jobject (32 bit pointer)
    PrintDebugString("##### WinAccessBridge::getAccessibleActions(%X, %p, %p)", vmID,
                     accessibleContext, actions);
#else // JOBJECT64 is jlong (64 bit)
    PrintDebugString("##### WinAccessBridge::getAccessibleActions(%X, %016I64X, %p)", vmID,
                     accessibleContext, actions);
#endif

    if ((AccessBridgeJavaVMInstance *) 0 == javaVMs) {
        return FALSE;
    }

    char buffer[sizeof(PackageType) + sizeof(GetAccessibleActionsPackage)];
    PackageType *type = (PackageType *) buffer;
    GetAccessibleActionsPackage *pkg = (GetAccessibleActionsPackage *) (buffer + sizeof(PackageType));
    *type = cGetAccessibleActionsPackage;
    pkg->vmID = vmID;
    pkg->accessibleContext = accessibleContext;

    HWND destABWindow = javaVMs->findAccessBridgeWindow(vmID);
    if (destABWindow != (HWND) 0) {
        if (sendMemoryPackage(buffer, sizeof(buffer), destABWindow) == TRUE) {
            memcpy(actions, &(pkg->rAccessibleActions), sizeof(AccessibleActions));

            PrintDebugString("  ##### actions.actionsCount = %d", actions->actionsCount);
            PrintDebugString("  ##### WinAccessBridge::getAccessibleActions succeeded");

            return TRUE;
        }
    }
    PrintDebugString("  ##### WinAccessBridge::getAccessibleActions failed");
    return FALSE;
}

BOOL
WinAccessBridge::doAccessibleActions(long vmID, JOBJECT64 accessibleContext,
                                     AccessibleActionsToDo *actionsToDo, jint *failure) {

#ifdef ACCESSBRIDGE_ARCH_LEGACY // JOBJECT64 is jobject (32 bit pointer)
    PrintDebugString("WinAccessBridge::doAccessibleActions(%p #actions %d %ls)", accessibleContext,
                     actionsToDo->actionsCount,
                     actionsToDo->actions[0].name);
#else // JOBJECT64 is jlong (64 bit)
    PrintDebugString("WinAccessBridge::doAccessibleActions(%016I64X #actions %d %ls)", accessibleContext,
                     actionsToDo->actionsCount,
                     actionsToDo->actions[0].name);
#endif

    if ((AccessBridgeJavaVMInstance *) 0 == javaVMs) {
        return FALSE;
    }
    char buffer[sizeof(PackageType) + sizeof(DoAccessibleActionsPackage)];
    PackageType *type = (PackageType *) buffer;
    DoAccessibleActionsPackage *pkg = (DoAccessibleActionsPackage *) (buffer + sizeof(PackageType));
    *type = cDoAccessibleActionsPackage;
    pkg->vmID = vmID;
    pkg->accessibleContext = accessibleContext;
    memcpy((void *)(&(pkg->actionsToDo)), (void *)actionsToDo, sizeof(AccessibleActionsToDo));
    pkg->failure = -1;

    HWND destABWindow = javaVMs->findAccessBridgeWindow(pkg->vmID);
    if (sendMemoryPackage(buffer, sizeof(buffer), destABWindow) == TRUE) {
        *failure = pkg->failure;
        return pkg->rResult;
    }
    PrintDebugString("  WinAccessBridge::doAccessibleActions returning FALSE (sendMemoryPackage failed)");
    return FALSE;
}

/* ====== Utility methods ====== */

/**
 * Sets a text field to the specified string. Returns whether successful.
 */
BOOL
WinAccessBridge::setTextContents (const long vmID, const AccessibleContext accessibleContext,
                                  const wchar_t *text) {

    if ((AccessBridgeJavaVMInstance *) 0 == javaVMs) {
        return FALSE;
    }
    char buffer[sizeof(PackageType) + sizeof(SetTextContentsPackage)];
    PackageType *type = (PackageType *) buffer;
    SetTextContentsPackage *pkg = (SetTextContentsPackage *) (buffer + sizeof(PackageType));
    *type = cSetTextContentsPackage;
    pkg->vmID = vmID;
    pkg->accessibleContext = accessibleContext;
    wcsncpy(pkg->text, text, sizeof(pkg->text)/sizeof(wchar_t)); // wide character copy

#ifdef ACCESSBRIDGE_ARCH_LEGACY // JOBJECT64 is jobject (32 bit pointer)
    PrintDebugString("WinAccessBridge::setTextContents(%X, %016I64X %ls)", vmID, accessibleContext, text);
#else // JOBJECT64 is jlong (64 bit)
    PrintDebugString("WinAccessBridge::setTextContents(%X, %p %ls)", vmID, accessibleContext, text);
#endif
    // need to call only the HWND/VM that contains this AC
    HWND destABWindow = javaVMs->findAccessBridgeWindow(vmID);
    if (destABWindow != (HWND) 0) {
        if (sendMemoryPackage(buffer, sizeof(buffer), destABWindow) == TRUE) {
            return pkg->rResult;
        }
    }
    return FALSE;
}

/**
 * Returns the Accessible Context of a Page Tab object that is the
 * ancestor of a given object.  If the object is a Page Tab object
 * or a Page Tab ancestor object was found, returns the object
 * AccessibleContext.
 * If there is no ancestor object that has an Accessible Role of Page Tab,
 * returns (AccessibleContext)0.
 */
AccessibleContext
WinAccessBridge::getParentWithRole (const long vmID, const AccessibleContext accessibleContext, const wchar_t *role) {

    if ((AccessBridgeJavaVMInstance *) 0 == javaVMs) {
        return (JOBJECT64)0;
    }
    char buffer[sizeof(PackageType) + sizeof(GetParentWithRolePackage)];
    PackageType *type = (PackageType *) buffer;
    GetParentWithRolePackage *pkg = (GetParentWithRolePackage *) (buffer + sizeof(PackageType));
    *type = cGetParentWithRolePackage;
    pkg->vmID = vmID;
    pkg->accessibleContext = accessibleContext;
    memcpy((void *)(&(pkg->role)), (void *)role, sizeof(pkg->role));

#ifdef ACCESSBRIDGE_ARCH_LEGACY // JOBJECT64 is jobject (32 bit pointer)
    PrintDebugString("WinAccessBridge::getParentWithRole(%X, %p)", vmID, accessibleContext);
#else // JOBJECT64 is jlong (64 bit)
    PrintDebugString("WinAccessBridge::getParentWithRole(%X, %016I64X)", vmID, accessibleContext);
#endif
    PrintDebugString("  pkg->vmID: %X", pkg->vmID);
    PrintDebugString("  pkg->accessibleContext: %p", pkg->accessibleContext);
    PrintDebugString("  pkg->role: %ls", pkg->role);
    // need to call only the HWND/VM that contains this AC
    HWND destABWindow = javaVMs->findAccessBridgeWindow(vmID);
    if (destABWindow != (HWND) 0) {
        if (sendMemoryPackage(buffer, sizeof(buffer), destABWindow) == TRUE) {
            PrintDebugString("  pkg->rAccessibleContext: %p", pkg->rAccessibleContext);
            return pkg->rAccessibleContext;
        }
    }
    return (JOBJECT64) 0;
}


/**
 * Returns the Accessible Context for the top level object in
 * a Java Window.  This is same Accessible Context that is obtained
 * from GetAccessibleContextFromHWND for that window.  Returns
 * (AccessibleContext)0 on error.
 */
AccessibleContext
WinAccessBridge::getTopLevelObject (const long vmID, const AccessibleContext accessibleContext) {

    if ((AccessBridgeJavaVMInstance *) 0 == javaVMs) {
        return (JOBJECT64)0;
    }
    char buffer[sizeof(PackageType) + sizeof(GetTopLevelObjectPackage)];
    PackageType *type = (PackageType *) buffer;
    GetTopLevelObjectPackage *pkg = (GetTopLevelObjectPackage *) (buffer + sizeof(PackageType));
    *type = cGetTopLevelObjectPackage;
    pkg->vmID = vmID;
    pkg->accessibleContext = accessibleContext;

#ifdef ACCESSBRIDGE_ARCH_LEGACY // JOBJECT64 is jobject (32 bit pointer)
    PrintDebugString("WinAccessBridge::getTopLevelObject(%X, %p)", vmID, accessibleContext);
#else // JOBJECT64 is jlong (64 bit)
    PrintDebugString("WinAccessBridge::getTopLevelObject(%X, %016I64X)", vmID, accessibleContext);
#endif
    // need to call only the HWND/VM that contains this AC
    HWND destABWindow = javaVMs->findAccessBridgeWindow(vmID);
    if (destABWindow != (HWND) 0) {
        if (sendMemoryPackage(buffer, sizeof(buffer), destABWindow) == TRUE) {
            return pkg->rAccessibleContext;
        }
    }
    return (JOBJECT64) 0;
}

/**
 * If there is an Ancestor object that has an Accessible Role of
 * Internal Frame, returns the Accessible Context of the Internal
 * Frame object.  Otherwise, returns the top level object for that
 * Java Window.  Returns (AccessibleContext)0 on error.
 */
AccessibleContext
WinAccessBridge::getParentWithRoleElseRoot (const long vmID, const AccessibleContext accessibleContext, const wchar_t *role) {

    if ((AccessBridgeJavaVMInstance *) 0 == javaVMs) {
        return (JOBJECT64)0;
    }
    char buffer[sizeof(PackageType) + sizeof(GetParentWithRoleElseRootPackage)];
    PackageType *type = (PackageType *) buffer;
    GetParentWithRoleElseRootPackage *pkg = (GetParentWithRoleElseRootPackage *) (buffer + sizeof(PackageType));
    *type = cGetParentWithRoleElseRootPackage;
    pkg->vmID = vmID;
    pkg->accessibleContext = accessibleContext;
    memcpy((void *)(&(pkg->role)), (void *)role, sizeof(pkg->role));

#ifdef ACCESSBRIDGE_ARCH_LEGACY // JOBJECT64 is jobject (32 bit pointer)
    PrintDebugString("WinAccessBridge::getParentWithRoleElseRoot(%X, %p)", vmID, accessibleContext);
#else // JOBJECT64 is jlong (64 bit)
    PrintDebugString("WinAccessBridge::getParentWithRoleElseRoot(%X, %016I64X)", vmID, accessibleContext);
#endif
    // need to call only the HWND/VM that contains this AC
    HWND destABWindow = javaVMs->findAccessBridgeWindow(vmID);
    if (destABWindow != (HWND) 0) {
        if (sendMemoryPackage(buffer, sizeof(buffer), destABWindow) == TRUE) {
            return pkg->rAccessibleContext;
        }
    }
    return (JOBJECT64) 0;
}

/**
 * Returns how deep in the object hierarchy a given object is.
 * The top most object in the object hierarchy has an object depth of 0.
 * Returns -1 on error.
 */
int
WinAccessBridge::getObjectDepth (const long vmID, const AccessibleContext accessibleContext) {

    if ((AccessBridgeJavaVMInstance *) 0 == javaVMs) {
        return -1;
    }
    char buffer[sizeof(PackageType) + sizeof(GetObjectDepthPackage)];
    PackageType *type = (PackageType *) buffer;
    GetObjectDepthPackage *pkg = (GetObjectDepthPackage *) (buffer + sizeof(PackageType));
    *type = cGetObjectDepthPackage;
    pkg->vmID = vmID;
    pkg->accessibleContext = accessibleContext;

#ifdef ACCESSBRIDGE_ARCH_LEGACY // JOBJECT64 is jobject (32 bit pointer)
    PrintDebugString("WinAccessBridge::getObjectDepth(%X, %p)", vmID, accessibleContext);
#else // JOBJECT64 is jlong (64 bit)
    PrintDebugString("WinAccessBridge::getObjectDepth(%X, %016I64X)", vmID, accessibleContext);
#endif
    // need to call only the HWND/VM that contains this AC
    HWND destABWindow = javaVMs->findAccessBridgeWindow(vmID);
    if (destABWindow != (HWND) 0) {
        if (sendMemoryPackage(buffer, sizeof(buffer), destABWindow) == TRUE) {
            return pkg->rResult;
        }
    }
    return -1;
}

/**
 * Returns the Accessible Context of the currently ActiveDescendent of an object.
 * Returns (AccessibleContext)0 on error.
 */
AccessibleContext
WinAccessBridge::getActiveDescendent (const long vmID, const AccessibleContext accessibleContext) {

    if ((AccessBridgeJavaVMInstance *) 0 == javaVMs) {
        return (JOBJECT64)0;
    }
    char buffer[sizeof(PackageType) + sizeof(GetActiveDescendentPackage)];
    PackageType *type = (PackageType *) buffer;
    GetActiveDescendentPackage *pkg = (GetActiveDescendentPackage *) (buffer + sizeof(PackageType));
    *type = cGetActiveDescendentPackage;
    pkg->vmID = vmID;
    pkg->accessibleContext = accessibleContext;

#ifdef ACCESSBRIDGE_ARCH_LEGACY // JOBJECT64 is jobject (32 bit pointer)
    PrintDebugString("WinAccessBridge::getActiveDescendent(%X, %p)", vmID, accessibleContext);
#else // JOBJECT64 is jlong (64 bit)
    PrintDebugString("WinAccessBridge::getActiveDescendent(%X, %016I64X)", vmID, accessibleContext);
#endif
    // need to call only the HWND/VM that contains this AC
    HWND destABWindow = javaVMs->findAccessBridgeWindow(vmID);
    if (destABWindow != (HWND) 0) {
        if (sendMemoryPackage(buffer, sizeof(buffer), destABWindow) == TRUE) {
            return pkg->rAccessibleContext;
        }
    }
    return (JOBJECT64) 0;
}

/**
 * Additional methods for Teton
 */

/**
 * Gets the AccessibleName for a component based upon the JAWS algorithm. Returns
 * whether successful.
 *
 * Bug ID 4916682 - Implement JAWS AccessibleName policy
 */
BOOL
WinAccessBridge::getVirtualAccessibleName(long vmID, AccessibleContext accessibleContext,
                                          wchar_t *name, int len) {

    if ((AccessBridgeJavaVMInstance *) 0 == javaVMs) {
        return FALSE;
    }
    char buffer[sizeof(PackageType) + sizeof(GetVirtualAccessibleNamePackage)];
    PackageType *type = (PackageType *) buffer;
    GetVirtualAccessibleNamePackage *pkg = (GetVirtualAccessibleNamePackage *) (buffer + sizeof(PackageType));
    *type = cGetVirtualAccessibleNamePackage;
    pkg->vmID = vmID;
    pkg->accessibleContext = accessibleContext;
    size_t max = (len > sizeof(pkg->rName)) ? sizeof(pkg->rName) : len;
    pkg->len = (int)max;

#ifdef ACCESSBRIDGE_ARCH_LEGACY // JOBJECT64 is jobject (32 bit pointer)
    PrintDebugString("WinAccessBridge::getVirtualAccessibleName(%X, %p)", vmID, accessibleContext);
#else // JOBJECT64 is jlong (64 bit)
    PrintDebugString("WinAccessBridge::getVirtualAccessibleName(%X, %016I64X)", vmID, accessibleContext);
#endif
    // need to call only the HWND/VM that contains this AC
    HWND destABWindow = javaVMs->findAccessBridgeWindow(vmID);
    if (destABWindow != (HWND) 0) {
        if (sendMemoryPackage(buffer, sizeof(buffer), destABWindow) == TRUE) {
            wcsncpy(name, pkg->rName, max);
            PrintDebugString("    WinAccessBridge::getVirtualAccessibleName: Virtual name = %ls", name);
            return TRUE;
        }
    }
    return FALSE;
}

/**
 * Request focus for a component. Returns whether successful;
 *
 * Bug ID 4944757 - requestFocus method needed
 */
BOOL
WinAccessBridge::requestFocus(long vmID, AccessibleContext accessibleContext) {

    if ((AccessBridgeJavaVMInstance *) 0 == javaVMs) {
        return FALSE;
    }
    char buffer[sizeof(PackageType) + sizeof(RequestFocusPackage)];
    PackageType *type = (PackageType *) buffer;
    RequestFocusPackage *pkg = (RequestFocusPackage *) (buffer + sizeof(PackageType));
    *type = cRequestFocusPackage;
    pkg->vmID = vmID;
    pkg->accessibleContext = accessibleContext;

#ifdef ACCESSBRIDGE_ARCH_LEGACY // JOBJECT64 is jobject (32 bit pointer)
    PrintDebugString("WinAccessBridge::requestFocus(%X, %p)", vmID, accessibleContext);
#else // JOBJECT64 is jlong (64 bit)
    PrintDebugString("WinAccessBridge::requestFocus(%X, %016I64X)", vmID, accessibleContext);
#endif
    // need to call only the HWND/VM that contains this AC
    HWND destABWindow = javaVMs->findAccessBridgeWindow(vmID);
    if (destABWindow != (HWND) 0) {
        if (sendMemoryPackage(buffer, sizeof(buffer), destABWindow) == TRUE) {
            return TRUE;
        }
    }
    return FALSE;
}

/**
 * Selects text between two indices.  Selection includes the text at the start index
 * and the text at the end index. Returns whether successful;
 *
 * Bug ID 4944758 - selectTextRange method needed
 */
BOOL
WinAccessBridge::selectTextRange(long vmID, AccessibleContext accessibleContext, int startIndex, int endIndex) {
    if ((AccessBridgeJavaVMInstance *) 0 == javaVMs) {
        return FALSE;
    }
    char buffer[sizeof(PackageType) + sizeof(SelectTextRangePackage)];
    PackageType *type = (PackageType *) buffer;
    SelectTextRangePackage *pkg = (SelectTextRangePackage *) (buffer + sizeof(PackageType));
    *type = cSelectTextRangePackage;
    pkg->vmID = vmID;
    pkg->accessibleContext = accessibleContext;
    pkg->startIndex = startIndex;
    pkg->endIndex = endIndex;

#ifdef ACCESSBRIDGE_ARCH_LEGACY // JOBJECT64 is jobject (32 bit pointer)
    PrintDebugString("    WinAccessBridge::selectTextRange(%X, %p %d %d)", vmID, accessibleContext,
                     startIndex, endIndex);
#else // JOBJECT64 is jlong (64 bit)
    PrintDebugString("    WinAccessBridge::selectTextRange(%X, %016I64X %d %d)", vmID, accessibleContext,
                     startIndex, endIndex);
#endif
    // need to call only the HWND/VM that contains this AC
    HWND destABWindow = javaVMs->findAccessBridgeWindow(vmID);
    if (destABWindow != (HWND) 0) {
        if (sendMemoryPackage(buffer, sizeof(buffer), destABWindow) == TRUE) {
            return TRUE;
        }
    }
    return FALSE;
}

/**
 * Get text attributes between two indices.  The attribute list includes the text at the
 * start index and the text at the end index. Returns whether successful;
 *
 * Bug ID 4944761 - getTextAttributes between two indices method needed
 */
BOOL
WinAccessBridge::getTextAttributesInRange(long vmID, AccessibleContext accessibleContext,
                                          int startIndex, int endIndex,
                                          AccessibleTextAttributesInfo *attributes, short *len) {

    if ((AccessBridgeJavaVMInstance *) 0 == javaVMs) {
        return FALSE;
    }
    char buffer[sizeof(PackageType) + sizeof(GetTextAttributesInRangePackage)];
    PackageType *type = (PackageType *) buffer;
    GetTextAttributesInRangePackage *pkg = (GetTextAttributesInRangePackage *) (buffer + sizeof(PackageType));
    *type = cGetTextAttributesInRangePackage;
    pkg->vmID = vmID;
    pkg->accessibleContext = accessibleContext;
    pkg->startIndex = startIndex;
    pkg->endIndex = endIndex;
    memcpy(&(pkg->attributes), attributes, sizeof(AccessibleTextAttributesInfo));


#ifdef ACCESSBRIDGE_ARCH_LEGACY // JOBJECT64 is jobject (32 bit pointer)
    PrintDebugString("    WinAccessBridge::getTextAttributesInRange(%X, %p %d %d)", vmID, accessibleContext,
                     startIndex, endIndex);
#else // JOBJECT64 is jlong (64 bit)
    PrintDebugString("    WinAccessBridge::getTextAttributesInRange(%X, %016I64X %d %d)", vmID, accessibleContext,
                     startIndex, endIndex);
#endif
    // need to call only the HWND/VM that contains this AC
    HWND destABWindow = javaVMs->findAccessBridgeWindow(vmID);
    if (destABWindow != (HWND) 0) {
        if (sendMemoryPackage(buffer, sizeof(buffer), destABWindow) == TRUE) {
            *attributes = pkg->attributes;
            *len = pkg->rLength;
            return TRUE;
        }
    }
    return FALSE;
}

/**
 * Gets the number of visible children of a component. Returns -1 on error.
 *
 * Bug ID 4944762- getVisibleChildren for list-like components needed
 */
int
WinAccessBridge::getVisibleChildrenCount(long vmID, AccessibleContext accessibleContext) {

    if ((AccessBridgeJavaVMInstance *) 0 == javaVMs) {
        return -1;
    }
    char buffer[sizeof(PackageType) + sizeof(GetVisibleChildrenCountPackage)];
    PackageType *type = (PackageType *) buffer;
    GetVisibleChildrenCountPackage *pkg = (GetVisibleChildrenCountPackage *) (buffer + sizeof(PackageType));
    *type = cGetVisibleChildrenCountPackage;
    pkg->vmID = vmID;
    pkg->accessibleContext = accessibleContext;

#ifdef ACCESSBRIDGE_ARCH_LEGACY // JOBJECT64 is jobject (32 bit pointer)
    PrintDebugString("WinAccessBridge::getVisibleChildrenCount(%X, %p)", vmID, accessibleContext);
#else // JOBJECT64 is jlong (64 bit)
    PrintDebugString("WinAccessBridge::getVisibleChildrenCount(%X, %016I64X)", vmID, accessibleContext);
#endif
    // need to call only the HWND/VM that contains this AC
    HWND destABWindow = javaVMs->findAccessBridgeWindow(vmID);
    if (destABWindow != (HWND) 0) {
        if (sendMemoryPackage(buffer, sizeof(buffer), destABWindow) == TRUE) {
            return pkg->rChildrenCount;
        }
    }
    return -1;
}

/**
 * Gets the visible children of an AccessibleContext. Returns whether successful;
 *
 * Bug ID 4944762- getVisibleChildren for list-like components needed
 */
BOOL
WinAccessBridge::getVisibleChildren(long vmID, AccessibleContext accessibleContext, int startIndex,
                                    VisibleChildrenInfo *visibleChildrenInfo) {

    if ((AccessBridgeJavaVMInstance *) 0 == javaVMs) {
        return FALSE;
    }
    char buffer[sizeof(PackageType) + sizeof(GetVisibleChildrenPackage)];
    PackageType *type = (PackageType *) buffer;
    GetVisibleChildrenPackage *pkg = (GetVisibleChildrenPackage *) (buffer + sizeof(PackageType));
    *type = cGetVisibleChildrenPackage;
    pkg->vmID = vmID;
    pkg->accessibleContext = accessibleContext;
    pkg->startIndex = startIndex;

#ifdef ACCESSBRIDGE_ARCH_LEGACY // JOBJECT64 is jobject (32 bit pointer)
    PrintDebugString("WinAccessBridge::getVisibleChildren(%X, %p)", vmID, accessibleContext);
#else // JOBJECT64 is jlong (64 bit)
    PrintDebugString("WinAccessBridge::getVisibleChildren(%X, %016I64X)", vmID, accessibleContext);
#endif
    // need to call only the HWND/VM that contains this AC
    HWND destABWindow = javaVMs->findAccessBridgeWindow(vmID);
    if (destABWindow != (HWND) 0) {
        if (sendMemoryPackage(buffer, sizeof(buffer), destABWindow) == TRUE) {
            memcpy(visibleChildrenInfo, &(pkg->rVisibleChildrenInfo), sizeof(pkg->rVisibleChildrenInfo));
            return pkg->rSuccess;
        }
    }
    return FALSE;
}

/**
 * Set the caret to a text position. Returns whether successful;
 *
 * Bug ID 4944770 - setCaretPosition method needed
 */
BOOL
WinAccessBridge::setCaretPosition(long vmID, AccessibleContext accessibleContext, int position) {

    if ((AccessBridgeJavaVMInstance *) 0 == javaVMs) {
        return FALSE;
    }
    char buffer[sizeof(PackageType) + sizeof(SetCaretPositionPackage)];
    PackageType *type = (PackageType *) buffer;
    SetCaretPositionPackage *pkg = (SetCaretPositionPackage *) (buffer + sizeof(PackageType));
    *type = cSetCaretPositionPackage;
    pkg->vmID = vmID;
    pkg->accessibleContext = accessibleContext;
    pkg->position = position;

#ifdef ACCESSBRIDGE_ARCH_LEGACY // JOBJECT64 is jobject (32 bit pointer)
    PrintDebugString("WinAccessBridge::setCaretPosition(%X, %p %ls)", vmID, accessibleContext);
#else // JOBJECT64 is jlong (64 bit)
    PrintDebugString("WinAccessBridge::setCaretPosition(%X, %016I64X %ls)", vmID, accessibleContext);
#endif
    // need to call only the HWND/VM that contains this AC
    HWND destABWindow = javaVMs->findAccessBridgeWindow(vmID);
    if (destABWindow != (HWND) 0) {
        if (sendMemoryPackage(buffer, sizeof(buffer), destABWindow) == TRUE) {
            return TRUE;
        }
    }
    return FALSE;
}


/********** AccessibleText routines ***********************************/

/**
 * getAccessibleTextInfo - fills a struct with a bunch of information
 * contained in the Java Accessibility AccessibleText API
 *
 *
 * Note: if the AccessibleContext parameter is bogus, this call will blow up
 */
BOOL
WinAccessBridge::getAccessibleTextInfo(long vmID,
                                       JOBJECT64 AccessibleContext,
                                       AccessibleTextInfo *textInfo,
                                       jint x, jint y) {
    if ((AccessBridgeJavaVMInstance *) 0 == javaVMs) {
        return FALSE;
    }
    char buffer[sizeof(PackageType) + sizeof(GetAccessibleTextInfoPackage)];
    PackageType *type = (PackageType *) buffer;
    GetAccessibleTextInfoPackage *pkg = (GetAccessibleTextInfoPackage *) (buffer + sizeof(PackageType));
    *type = cGetAccessibleTextInfoPackage;
    pkg->vmID = vmID;
    pkg->AccessibleContext = AccessibleContext;
    pkg->x = x;
    pkg->y = y;

#ifdef ACCESSBRIDGE_ARCH_LEGACY // JOBJECT64 is jobject (32 bit pointer)
    PrintDebugString("WinAccessBridge::getAccessibleTextInfo(%X, %p, %p, %d, %d)", vmID, AccessibleContext, textInfo, x, y);
#else // JOBJECT64 is jlong (64 bit)
    PrintDebugString("WinAccessBridge::getAccessibleTextInfo(%X, %016I64X, %p, %d, %d)", vmID, AccessibleContext, textInfo, x, y);
#endif
    // need to call only the HWND/VM that contains this AC
    HWND destABWindow = javaVMs->findAccessBridgeWindow(vmID);
    if (destABWindow != (HWND) 0) {
        if (sendMemoryPackage(buffer, sizeof(buffer), destABWindow) == TRUE) {
            memcpy(textInfo, &(pkg->rTextInfo), sizeof(AccessibleTextInfo));
            if (pkg->rTextInfo.charCount != -1) {
                PrintDebugString("  charCount: %d", textInfo->charCount);
                PrintDebugString("  caretIndex: %d", textInfo->caretIndex);
                PrintDebugString("  indexAtPoint: %d", textInfo->indexAtPoint);
                return TRUE;
            }
        }
    }

    return FALSE;
}

/**
 * getAccessibleTextItems - fills a struct with letter, word, and sentence info
 * of the AccessibleText interface at a given index
 *
 * Note: if the AccessibleContext parameter is bogus, this call will blow up
 */
BOOL
WinAccessBridge::getAccessibleTextItems(long vmID,
                                        JOBJECT64 AccessibleContext,
                                        AccessibleTextItemsInfo *textItems,
                                        jint index) {
    if ((AccessBridgeJavaVMInstance *) 0 == javaVMs) {
        return FALSE;
    }
    char buffer[sizeof(PackageType) + sizeof(GetAccessibleTextItemsPackage)];
    PackageType *type = (PackageType *) buffer;
    GetAccessibleTextItemsPackage *pkg = (GetAccessibleTextItemsPackage *) (buffer + sizeof(PackageType));
    *type = cGetAccessibleTextItemsPackage;
    pkg->vmID = vmID;
    pkg->AccessibleContext = AccessibleContext;
    pkg->index = index;
    // zero things out, in case the call fails
    pkg->rTextItemsInfo.letter = '\0';
    pkg->rTextItemsInfo.word[0] = '\0';
    pkg->rTextItemsInfo.sentence[0] = '\0';

#ifdef ACCESSBRIDGE_ARCH_LEGACY // JOBJECT64 is jobject (32 bit pointer)
    PrintDebugString("WinAccessBridge::getAccessibleTextItems(%X, %p, %p, %d)", vmID, AccessibleContext, textItems, index);
#else // JOBJECT64 is jlong (64 bit)
    PrintDebugString("WinAccessBridge::getAccessibleTextItems(%X, %016I64X, %p, %d)", vmID, AccessibleContext, textItems, index);
#endif
    // need to call only the HWND/VM that contains this AC
    HWND destABWindow = javaVMs->findAccessBridgeWindow(vmID);
    if (destABWindow != (HWND) 0) {
        if (sendMemoryPackage(buffer, sizeof(buffer), destABWindow) == TRUE) {
            memcpy(textItems, &(pkg->rTextItemsInfo), sizeof(AccessibleTextItemsInfo));
            if (pkg->rTextItemsInfo.letter != '/0') {
                return TRUE;
            }
        }
    }

    return FALSE;
}

/**
 * getAccessibleTextSelectionInfo - returns information about the selected
 * text of the object implementing AccessibleText
 *
 * Note: if the AccessibleContext parameter is bogus, this call will blow up
 */
BOOL
WinAccessBridge::getAccessibleTextSelectionInfo(long vmID,
                                                JOBJECT64 AccessibleContext,
                                                AccessibleTextSelectionInfo *selectionInfo) {
    if ((AccessBridgeJavaVMInstance *) 0 == javaVMs) {
        return FALSE;
    }
    char buffer[sizeof(PackageType) + sizeof(GetAccessibleTextSelectionInfoPackage)];
    PackageType *type = (PackageType *) buffer;
    GetAccessibleTextSelectionInfoPackage *pkg = (GetAccessibleTextSelectionInfoPackage *) (buffer + sizeof(PackageType));
    *type = cGetAccessibleTextSelectionInfoPackage;
    pkg->vmID = vmID;
    pkg->AccessibleContext = AccessibleContext;

#ifdef ACCESSBRIDGE_ARCH_LEGACY // JOBJECT64 is jobject (32 bit pointer)
    PrintDebugString("WinAccessBridge::getAccessibleTextSelectionInfo(%X, %p, %p)", vmID, AccessibleContext, selectionInfo);
#else // JOBJECT64 is jlong (64 bit)
    PrintDebugString("WinAccessBridge::getAccessibleTextSelectionInfo(%X, %016I64X, %p)", vmID, AccessibleContext, selectionInfo);
#endif
    // need to call only the HWND/VM that contains this AC
    HWND destABWindow = javaVMs->findAccessBridgeWindow(vmID);
    if (destABWindow != (HWND) 0) {
        if (sendMemoryPackage(buffer, sizeof(buffer), destABWindow) == TRUE) {
            memcpy(selectionInfo, &(pkg->rTextSelectionItemsInfo), sizeof(AccessibleTextSelectionInfo));
            // [[[FIXME]]] should test to see if valid info returned; return FALSE if not
            return TRUE;
        }
    }

    return FALSE;
}

/**
 * getAccessibleTextAttributes - performs the Java code:
 *   ...[[[FIXME]]] fill in this comment...
 *
 * Note: if the AccessibleContext parameter is bogus, this call will blow up
 */
BOOL
WinAccessBridge::getAccessibleTextAttributes(long vmID,
                                             JOBJECT64 AccessibleContext,
                                             jint index,
                                             AccessibleTextAttributesInfo *attributes) {
    if ((AccessBridgeJavaVMInstance *) 0 == javaVMs) {
        return FALSE;
    }
    char buffer[sizeof(PackageType) + sizeof(GetAccessibleTextAttributeInfoPackage)];
    PackageType *type = (PackageType *) buffer;
    GetAccessibleTextAttributeInfoPackage *pkg = (GetAccessibleTextAttributeInfoPackage *) (buffer + sizeof(PackageType));
    *type = cGetAccessibleTextAttributeInfoPackage;
    pkg->vmID = vmID;
    pkg->AccessibleContext = AccessibleContext;
    pkg->index = index;

#ifdef ACCESSBRIDGE_ARCH_LEGACY // JOBJECT64 is jobject (32 bit pointer)
    PrintDebugString("WinAccessBridge::getAccessibleTextAttributes(%X, %p, %d, %p)", vmID, AccessibleContext, index, attributes);
#else // JOBJECT64 is jlong (64 bit)
    PrintDebugString("WinAccessBridge::getAccessibleTextAttributes(%X, %016I64X, %d, %p)", vmID, AccessibleContext, index, attributes);
#endif
    // need to call only the HWND/VM that contains this AC
    HWND destABWindow = javaVMs->findAccessBridgeWindow(vmID);
    if (destABWindow != (HWND) 0) {
        if (sendMemoryPackage(buffer, sizeof(buffer), destABWindow) == TRUE) {
            memcpy(attributes, &(pkg->rAttributeInfo), sizeof(AccessibleTextAttributesInfo));
            return TRUE;
        }
    }

    return FALSE;
}

/**
 * getAccessibleTextRect - gets the text bounding rectangle
 *
 * Note: if the AccessibleContext parameter is bogus, this call will blow up
 */
BOOL
WinAccessBridge::getAccessibleTextRect(long vmID,
                                       JOBJECT64 AccessibleContext,
                                       AccessibleTextRectInfo *rectInfo,
                                       jint index) {
    if ((AccessBridgeJavaVMInstance *) 0 == javaVMs) {
        return FALSE;
    }
    char buffer[sizeof(PackageType) + sizeof(GetAccessibleTextRectInfoPackage)];
    PackageType *type = (PackageType *) buffer;
    GetAccessibleTextRectInfoPackage *pkg = (GetAccessibleTextRectInfoPackage *) (buffer + sizeof(PackageType));
    *type = cGetAccessibleTextRectInfoPackage;
    pkg->vmID = vmID;
    pkg->AccessibleContext = AccessibleContext;
    pkg->index = index;

#ifdef ACCESSBRIDGE_ARCH_LEGACY // JOBJECT64 is jobject (32 bit pointer)
    PrintDebugString("WinAccessBridge::getAccessibleTextRect(%X, %p, %p, %d)", vmID, AccessibleContext, rectInfo, index);
#else // JOBJECT64 is jlong (64 bit)
    PrintDebugString("WinAccessBridge::getAccessibleTextRect(%X, %016I64X, %p, %d)", vmID, AccessibleContext, rectInfo, index);
#endif
    // need to call only the HWND/VM that contains this AC
    HWND destABWindow = javaVMs->findAccessBridgeWindow(vmID);
    if (destABWindow != (HWND) 0) {
        if (sendMemoryPackage(buffer, sizeof(buffer), destABWindow) == TRUE) {
            memcpy(rectInfo, (&pkg->rTextRectInfo), sizeof(AccessibleTextRectInfo));
            // [[[FIXME]]] should test to see if valid info returned; return FALSE if not
            return TRUE;
        }
    }

    return FALSE;
}


/**
 * getAccessibleTextRect - gets the text bounding rectangle
 *
 * Note: if the AccessibleContext parameter is bogus, this call will blow up
 */
BOOL
WinAccessBridge::getCaretLocation(long vmID,
                                       JOBJECT64 AccessibleContext,
                                       AccessibleTextRectInfo *rectInfo,
                                       jint index) {
    if ((AccessBridgeJavaVMInstance *) 0 == javaVMs) {
        return FALSE;
    }
    char buffer[sizeof(PackageType) + sizeof(GetCaretLocationPackage)];
    PackageType *type = (PackageType *) buffer;
    GetCaretLocationPackage *pkg = (GetCaretLocationPackage *) (buffer + sizeof(PackageType));
    *type = cGetCaretLocationPackage;
    pkg->vmID = vmID;
    pkg->AccessibleContext = AccessibleContext;
    pkg->index = index;

#ifdef ACCESSBRIDGE_ARCH_LEGACY // JOBJECT64 is jobject (32 bit pointer)
    PrintDebugString("WinAccessBridge::getCaretLocation(%X, %p, %p, %d)", vmID, AccessibleContext, rectInfo, index);
#else // JOBJECT64 is jlong (64 bit)
    PrintDebugString("WinAccessBridge::getCaretLocation(%X, %016I64X, %p, %d)", vmID, AccessibleContext, rectInfo, index);
#endif
    // need to call only the HWND/VM that contains this AC
    HWND destABWindow = javaVMs->findAccessBridgeWindow(vmID);
    if (destABWindow != (HWND) 0) {
        if (sendMemoryPackage(buffer, sizeof(buffer), destABWindow) == TRUE) {
            memcpy(rectInfo, (&pkg->rTextRectInfo), sizeof(AccessibleTextRectInfo));
            return TRUE;
        }
    }

    return FALSE;
}


/**
 * getEventsWaiting - gets the number of events waiting to fire
 *
 * Note: if the AccessibleContext parameter is bogus, this call will blow up
 */
int
WinAccessBridge::getEventsWaiting() {
    if(messageQueue) {
        return(messageQueue->getEventsWaiting());
    }
    return(0);
}


/**
 * getAccessibleTextLineBounds - gets the bounding rectangle for the text line
 *
 * Note: if the AccessibleContext parameter is bogus, this call will blow up
 */
BOOL
WinAccessBridge::getAccessibleTextLineBounds(long vmID,
                                             JOBJECT64 AccessibleContext,
                                             jint index, jint *startIndex, jint *endIndex) {
    if ((AccessBridgeJavaVMInstance *) 0 == javaVMs) {
        return FALSE;
    }
    char buffer[sizeof(PackageType) + sizeof(GetAccessibleTextLineBoundsPackage)];
    PackageType *type = (PackageType *) buffer;
    GetAccessibleTextLineBoundsPackage *pkg = (GetAccessibleTextLineBoundsPackage *) (buffer + sizeof(PackageType));
    *type = cGetAccessibleTextLineBoundsPackage;
    pkg->vmID = vmID;
    pkg->AccessibleContext = AccessibleContext;
    pkg->index = index;

#ifdef ACCESSBRIDGE_ARCH_LEGACY // JOBJECT64 is jobject (32 bit pointer)
    PrintDebugString("WinAccessBridge::getAccessibleTextLineBounds(%X, %p, %d, )", vmID, AccessibleContext, index);
#else // JOBJECT64 is jlong (64 bit)
    PrintDebugString("WinAccessBridge::getAccessibleTextLineBounds(%X, %016I64X, %d, )", vmID, AccessibleContext, index);
#endif
    // need to call only the HWND/VM that contains this AC
    HWND destABWindow = javaVMs->findAccessBridgeWindow(vmID);
    if (destABWindow != (HWND) 0) {
        if (sendMemoryPackage(buffer, sizeof(buffer), destABWindow) == TRUE) {
            *startIndex = pkg->rLineStart;
            *endIndex = pkg->rLineEnd;
            // [[[FIXME]]] should test to see if valid info returned; return FALSE if not
            return TRUE;
        }
    }

    return FALSE;
}


/**
 * getAccessibleTextLineBounds - performs the Java code:
 *   ...[[[FIXME]]] fill in this comment...
 *
 * Note: if the AccessibleContext parameter is bogus, this call will blow up
 */
BOOL
WinAccessBridge::getAccessibleTextRange(long vmID,
                                        JOBJECT64 AccessibleContext,
                                        jint start, jint end, wchar_t *text, short len) {
    if ((AccessBridgeJavaVMInstance *) 0 == javaVMs) {
        return FALSE;
    }
    char buffer[sizeof(PackageType) + sizeof(GetAccessibleTextRangePackage)];
    PackageType *type = (PackageType *) buffer;
    GetAccessibleTextRangePackage *pkg = (GetAccessibleTextRangePackage *) (buffer + sizeof(PackageType));
    *type = cGetAccessibleTextRangePackage;
    pkg->vmID = vmID;
    pkg->AccessibleContext = AccessibleContext;
    pkg->start = start;
    pkg->end = end;

#ifdef ACCESSBRIDGE_ARCH_LEGACY // JOBJECT64 is jobject (32 bit pointer)
    PrintDebugString("WinAccessBridge::getAccessibleTextRange(%X, %p, %d, %d, )", vmID, AccessibleContext, start, end);
#else // JOBJECT64 is jlong (64 bit)
    PrintDebugString("WinAccessBridge::getAccessibleTextRange(%X, %016I64X, %d, %d, )", vmID, AccessibleContext, start, end);
#endif
    // need to call only the HWND/VM that contains this AC
    HWND destABWindow = javaVMs->findAccessBridgeWindow(vmID);
    if (destABWindow != (HWND) 0) {
        if (sendMemoryPackage(buffer, sizeof(buffer), destABWindow) == TRUE) {
            wcsncpy(text, pkg->rText, len);
            // [[[FIXME]]] should test to see if valid info returned; return FALSE if not
            return TRUE;
        }
    }

    return FALSE;
}




/********** AccessibleValue routines ***************/

BOOL
WinAccessBridge::getCurrentAccessibleValueFromContext(long vmID,
                                                      JOBJECT64 AccessibleContext,
                                                      wchar_t *value, short len) {
    if ((AccessBridgeJavaVMInstance *) 0 == javaVMs) {
        return FALSE;
    }
    char buffer[sizeof(PackageType) + sizeof(GetCurrentAccessibleValueFromContextPackage)];
    PackageType *type = (PackageType *) buffer;
    GetCurrentAccessibleValueFromContextPackage *pkg = (GetCurrentAccessibleValueFromContextPackage *) (buffer + sizeof(PackageType));
    *type = cGetCurrentAccessibleValueFromContextPackage;
    pkg->vmID = vmID;
    pkg->AccessibleContext = AccessibleContext;

    // need to call only the HWND/VM that contains this AC
    HWND destABWindow = javaVMs->findAccessBridgeWindow(vmID);
    if (destABWindow != (HWND) 0) {
        if (sendMemoryPackage(buffer, sizeof(buffer), destABWindow) == TRUE) {
            wcsncpy(value, pkg->rValue, len);
            // [[[FIXME]]] should test to see if valid info returned; return FALSE if not
            return TRUE;
        }
    }

    return FALSE;
}

BOOL
WinAccessBridge::getMaximumAccessibleValueFromContext(long vmID,
                                                      JOBJECT64 AccessibleContext,
                                                      wchar_t *value, short len) {
    if ((AccessBridgeJavaVMInstance *) 0 == javaVMs) {
        return FALSE;
    }
    char buffer[sizeof(PackageType) + sizeof(GetMaximumAccessibleValueFromContextPackage)];
    PackageType *type = (PackageType *) buffer;
    GetMaximumAccessibleValueFromContextPackage *pkg = (GetMaximumAccessibleValueFromContextPackage *) (buffer + sizeof(PackageType));
    *type = cGetMaximumAccessibleValueFromContextPackage;
    pkg->vmID = vmID;
    pkg->AccessibleContext = AccessibleContext;

    // need to call only the HWND/VM that contains this AC
    HWND destABWindow = javaVMs->findAccessBridgeWindow(vmID);
    if (destABWindow != (HWND) 0) {
        if (sendMemoryPackage(buffer, sizeof(buffer), destABWindow) == TRUE) {
            wcsncpy(value, pkg->rValue, len);
            // [[[FIXME]]] should test to see if valid info returned; return FALSE if not
            return TRUE;
        }
    }

    return FALSE;
}

BOOL
WinAccessBridge::getMinimumAccessibleValueFromContext(long vmID,
                                                      JOBJECT64 AccessibleContext,
                                                      wchar_t *value, short len) {
    if ((AccessBridgeJavaVMInstance *) 0 == javaVMs) {
        return FALSE;
    }
    char buffer[sizeof(PackageType) + sizeof(GetMinimumAccessibleValueFromContextPackage)];
    PackageType *type = (PackageType *) buffer;
    GetMinimumAccessibleValueFromContextPackage *pkg = (GetMinimumAccessibleValueFromContextPackage *) (buffer + sizeof(PackageType));
    *type = cGetMinimumAccessibleValueFromContextPackage;
    pkg->vmID = vmID;
    pkg->AccessibleContext = AccessibleContext;

    // need to call only the HWND/VM that contains this AC
    HWND destABWindow = javaVMs->findAccessBridgeWindow(vmID);
    if (destABWindow != (HWND) 0) {
        if (sendMemoryPackage(buffer, sizeof(buffer), destABWindow) == TRUE) {
            wcsncpy(value, pkg->rValue, len);
            // [[[FIXME]]] should test to see if valid info returned; return FALSE if not
            return TRUE;
        }
    }

    return FALSE;
}


/********** AccessibleSelection routines ***************/

void
WinAccessBridge::addAccessibleSelectionFromContext(long vmID,
                                                   JOBJECT64 AccessibleContext, int i) {
    if ((AccessBridgeJavaVMInstance *) 0 == javaVMs) {
        return;
    }
    char buffer[sizeof(PackageType) + sizeof(AddAccessibleSelectionFromContextPackage)];
    PackageType *type = (PackageType *) buffer;
    AddAccessibleSelectionFromContextPackage *pkg = (AddAccessibleSelectionFromContextPackage *) (buffer + sizeof(PackageType));
    *type = cAddAccessibleSelectionFromContextPackage;
    pkg->vmID = vmID;
    pkg->AccessibleContext = AccessibleContext;
    pkg->index = i;

    // need to call only the HWND/VM that contains this AC
    HWND destABWindow = javaVMs->findAccessBridgeWindow(vmID);
    if (destABWindow != (HWND) 0) {
        sendMemoryPackage(buffer, sizeof(buffer), destABWindow);
    }
}

void
WinAccessBridge::clearAccessibleSelectionFromContext(long vmID,
                                                     JOBJECT64 AccessibleContext) {
    if ((AccessBridgeJavaVMInstance *) 0 == javaVMs) {
        return;
    }
    char buffer[sizeof(PackageType) + sizeof(ClearAccessibleSelectionFromContextPackage)];
    PackageType *type = (PackageType *) buffer;
    ClearAccessibleSelectionFromContextPackage *pkg = (ClearAccessibleSelectionFromContextPackage *) (buffer + sizeof(PackageType));
    *type = cClearAccessibleSelectionFromContextPackage;
    pkg->vmID = vmID;
    pkg->AccessibleContext = AccessibleContext;

    // need to call only the HWND/VM that contains this AC
    HWND destABWindow = javaVMs->findAccessBridgeWindow(vmID);
    if (destABWindow != (HWND) 0) {
        sendMemoryPackage(buffer, sizeof(buffer), destABWindow);
    }
}

JOBJECT64
WinAccessBridge::getAccessibleSelectionFromContext(long vmID,
                                                   JOBJECT64 AccessibleContext, int i) {
    if ((AccessBridgeJavaVMInstance *) 0 == javaVMs) {
        return (JOBJECT64)0;
    }
    char buffer[sizeof(PackageType) + sizeof(GetAccessibleSelectionFromContextPackage)];
    PackageType *type = (PackageType *) buffer;
    GetAccessibleSelectionFromContextPackage *pkg = (GetAccessibleSelectionFromContextPackage *) (buffer + sizeof(PackageType));
    *type = cGetAccessibleSelectionFromContextPackage;
    pkg->vmID = vmID;
    pkg->AccessibleContext = AccessibleContext;
    pkg->index = i;

    // need to call only the HWND/VM that contains this AC
    HWND destABWindow = javaVMs->findAccessBridgeWindow(vmID);
    if (destABWindow != (HWND) 0) {
        if (sendMemoryPackage(buffer, sizeof(buffer), destABWindow) == TRUE) {
            return pkg->rAccessibleContext;
        }
    }

    return (JOBJECT64) 0;
}

int
WinAccessBridge::getAccessibleSelectionCountFromContext(long vmID,
                                                        JOBJECT64 AccessibleContext) {
    if ((AccessBridgeJavaVMInstance *) 0 == javaVMs) {
        return -1;
    }
    char buffer[sizeof(PackageType) + sizeof(GetAccessibleSelectionCountFromContextPackage)];
    PackageType *type = (PackageType *) buffer;
    GetAccessibleSelectionCountFromContextPackage *pkg = (GetAccessibleSelectionCountFromContextPackage *) (buffer + sizeof(PackageType));
    *type = cGetAccessibleSelectionCountFromContextPackage;
    pkg->vmID = vmID;
    pkg->AccessibleContext = AccessibleContext;

    // need to call only the HWND/VM that contains this AC
    HWND destABWindow = javaVMs->findAccessBridgeWindow(vmID);
    if (destABWindow != (HWND) 0) {
        if (sendMemoryPackage(buffer, sizeof(buffer), destABWindow) == TRUE) {
            return (int) pkg->rCount;
        }
    }

    return -1;
}

BOOL
WinAccessBridge::isAccessibleChildSelectedFromContext(long vmID,
                                                      JOBJECT64 AccessibleContext, int i) {
    if ((AccessBridgeJavaVMInstance *) 0 == javaVMs) {
        return FALSE;
    }
    char buffer[sizeof(PackageType) + sizeof(IsAccessibleChildSelectedFromContextPackage)];
    PackageType *type = (PackageType *) buffer;
    IsAccessibleChildSelectedFromContextPackage *pkg = (IsAccessibleChildSelectedFromContextPackage *) (buffer + sizeof(PackageType));
    *type = cIsAccessibleChildSelectedFromContextPackage;
    pkg->vmID = vmID;
    pkg->AccessibleContext = AccessibleContext;
    pkg->index = i;

    // need to call only the HWND/VM that contains this AC
    HWND destABWindow = javaVMs->findAccessBridgeWindow(vmID);
    if (destABWindow != (HWND) 0) {
        if (sendMemoryPackage(buffer, sizeof(buffer), destABWindow) == TRUE) {
            if (pkg->rResult != 0) {
                return TRUE;
            }
        }
    }

    return FALSE;
}


void
WinAccessBridge::removeAccessibleSelectionFromContext(long vmID,
                                                      JOBJECT64 AccessibleContext, int i) {
    if ((AccessBridgeJavaVMInstance *) 0 == javaVMs) {
        return;
    }
    char buffer[sizeof(PackageType) + sizeof(RemoveAccessibleSelectionFromContextPackage)];
    PackageType *type = (PackageType *) buffer;
    RemoveAccessibleSelectionFromContextPackage *pkg = (RemoveAccessibleSelectionFromContextPackage *) (buffer + sizeof(PackageType));
    *type = cRemoveAccessibleSelectionFromContextPackage;
    pkg->vmID = vmID;
    pkg->AccessibleContext = AccessibleContext;
    pkg->index = i;

    // need to call only the HWND/VM that contains this AC
    HWND destABWindow = javaVMs->findAccessBridgeWindow(vmID);
    if (destABWindow != (HWND) 0) {
        sendMemoryPackage(buffer, sizeof(buffer), destABWindow);
    }
}

void
WinAccessBridge::selectAllAccessibleSelectionFromContext(long vmID,
                                                         JOBJECT64 AccessibleContext) {
    if ((AccessBridgeJavaVMInstance *) 0 == javaVMs) {
        return;
    }
    char buffer[sizeof(PackageType) + sizeof(SelectAllAccessibleSelectionFromContextPackage)];
    PackageType *type = (PackageType *) buffer;
    SelectAllAccessibleSelectionFromContextPackage *pkg = (SelectAllAccessibleSelectionFromContextPackage *) (buffer + sizeof(PackageType));
    *type = cSelectAllAccessibleSelectionFromContextPackage;
    pkg->vmID = vmID;
    pkg->AccessibleContext = AccessibleContext;

    // need to call only the HWND/VM that contains this AC
    HWND destABWindow = javaVMs->findAccessBridgeWindow(vmID);
    if (destABWindow != (HWND) 0) {
        sendMemoryPackage(buffer, sizeof(buffer), destABWindow);
    }
}


/*********** Event handling methods **********************************/

/**
 * addEventNotification - tell all Java-launched AccessBridge DLLs
 *                        that we want events of the specified type
 *
 * [[[FIXME]]] since we're just sending a long & a source window,
 *                         we could use a private message rather than WM_COPYDATA
 *                         (though we still may want it to be synchronous; dunno...)
 *
 */
void
WinAccessBridge::addJavaEventNotification(jlong type) {
    PrintDebugString("WinAccessBridge::addJavaEventNotification(%016I64X)", type);
    if ((AccessBridgeJavaVMInstance *) 0 == javaVMs) {
        return;
    }

    char buffer[sizeof(PackageType) + sizeof(AddJavaEventNotificationPackage)];
    PackageType *pkgType = (PackageType *) buffer;
    AddJavaEventNotificationPackage *pkg = (AddJavaEventNotificationPackage *) (buffer + sizeof(PackageType));
    *pkgType = cAddJavaEventNotificationPackage;
    pkg->type = type;
    pkg->DLLwindow = ABHandleToLong(dialogWindow);

    PrintDebugString("  ->pkgType = %X, eventType = %016I64X, DLLwindow = %p",
                     *pkgType, pkg->type, pkg->DLLwindow);

    // send addEventNotification message to all JVMs
    isVMInstanceChainInUse = true;
    AccessBridgeJavaVMInstance *current = javaVMs;
    while (current != (AccessBridgeJavaVMInstance *) 0) {
        current->sendPackage(buffer, sizeof(buffer));           // no return values!
        current = current->nextJVMInstance;
    }
    isVMInstanceChainInUse = false;
}

/**
 * removeEventNotification - tell all Java-launched AccessBridge DLLs
 *                                                       that we no longer want events of the
 *                                                       specified type
 *
 * [[[FIXME]]] since we're just sending a long & a source window,
 *                         we could use a private message rather than WM_COPYDATA
 *                         (though we still may want it to be synchronous; dunno...)
 *
 */
void
WinAccessBridge::removeJavaEventNotification(jlong type) {
    PrintDebugString("in WinAccessBridge::removeJavaEventNotification(%016I64X)", type);
    if ((AccessBridgeJavaVMInstance *) 0 == javaVMs) {
        return;
    }
    char buffer[sizeof(PackageType) + sizeof(RemoveJavaEventNotificationPackage)];
    PackageType *pkgType = (PackageType *) buffer;
    RemoveJavaEventNotificationPackage *pkg = (RemoveJavaEventNotificationPackage *) (buffer + sizeof(PackageType));
    *pkgType = cRemoveJavaEventNotificationPackage;
    pkg->type = type;
    pkg->DLLwindow = ABHandleToLong(dialogWindow);

    PrintDebugString("  ->pkgType = %X, eventType = %016I64X, DLLwindow = %p",
                     *pkgType, pkg->type, pkg->DLLwindow);

    // send removeEventNotification message to all JVMs
    isVMInstanceChainInUse = true;
    AccessBridgeJavaVMInstance *current = javaVMs;
    while (current != (AccessBridgeJavaVMInstance *) 0) {
        current->sendPackage(buffer, sizeof(buffer));           // no return values!
        current = current->nextJVMInstance;
    }
    isVMInstanceChainInUse = false;
}


/*********** Event handling methods **********************************/

/**
 * addAccessibilityEventNotification - tell all Java-launched AccessBridge DLLs
 *                        that we want events of the specified type
 *
 * [[[FIXME]]] since we're just sending a long & a source window,
 *                         we could use a private message rather than WM_COPYDATA
 *                         (though we still may want it to be synchronous; dunno...)
 *
 */
void
WinAccessBridge::addAccessibilityEventNotification(jlong type) {
    PrintDebugString("in WinAccessBridge::addAccessibilityEventNotification(%016I64X)", type);
    if ((AccessBridgeJavaVMInstance *) 0 == javaVMs) {
        return;
    }
    char buffer[sizeof(PackageType) + sizeof(AddAccessibilityEventNotificationPackage)];
    PackageType *pkgType = (PackageType *) buffer;
    AddAccessibilityEventNotificationPackage *pkg = (AddAccessibilityEventNotificationPackage *) (buffer + sizeof(PackageType));
    *pkgType = cAddAccessibilityEventNotificationPackage;
    pkg->type = type;
    pkg->DLLwindow = ABHandleToLong(dialogWindow);

    PrintDebugString("  ->pkgType = %X, eventType = %016I64X, DLLwindow = %X",
                     *pkgType, pkg->type, pkg->DLLwindow);

    // send addEventNotification message to all JVMs
    isVMInstanceChainInUse = true;
    AccessBridgeJavaVMInstance *current = javaVMs;
    while (current != (AccessBridgeJavaVMInstance *) 0) {
        current->sendPackage(buffer, sizeof(buffer));           // no return values!
        current = current->nextJVMInstance;
    }
    isVMInstanceChainInUse = false;
}

/**
 * removeAccessibilityEventNotification - tell all Java-launched AccessBridge DLLs
 *                                                       that we no longer want events of the
 *                                                       specified type
 *
 * [[[FIXME]]] since we're just sending a long & a source window,
 *                         we could use a private message rather than WM_COPYDATA
 *                         (though we still may want it to be synchronous; dunno...)
 *
 */
void
WinAccessBridge::removeAccessibilityEventNotification(jlong type) {
    PrintDebugString("in WinAccessBridge::removeAccessibilityEventNotification(%016I64X)", type);
    if ((AccessBridgeJavaVMInstance *) 0 == javaVMs) {
        return;
    }
    char buffer[sizeof(PackageType) + sizeof(RemoveAccessibilityEventNotificationPackage)];
    PackageType *pkgType = (PackageType *) buffer;
    RemoveAccessibilityEventNotificationPackage *pkg = (RemoveAccessibilityEventNotificationPackage *) (buffer + sizeof(PackageType));
    *pkgType = cRemoveAccessibilityEventNotificationPackage;
    pkg->type = type;
    pkg->DLLwindow = ABHandleToLong(dialogWindow);

    PrintDebugString("  ->pkgType = %X, eventType = %016I64X, DLLwindow = %X",
                     *pkgType, pkg->type, pkg->DLLwindow);

    // send removeEventNotification message to all JVMs
    isVMInstanceChainInUse = true;
    AccessBridgeJavaVMInstance *current = javaVMs;
    while (current != (AccessBridgeJavaVMInstance *) 0) {
        current->sendPackage(buffer, sizeof(buffer));           // no return values!
        current = current->nextJVMInstance;
    }
    isVMInstanceChainInUse = false;
}


#define CALL_SET_EVENT_FP(function, callbackFP)         \
        void WinAccessBridge::function(callbackFP fp) { \
                eventHandler->function(fp, this);                       \
                /* eventHandler calls back to winAccessBridgeDLL to set eventMask */    \
        }

    void WinAccessBridge::setJavaShutdownFP(AccessBridge_JavaShutdownFP fp) {
        eventHandler->setJavaShutdownFP(fp, this);
    }

    CALL_SET_EVENT_FP(setPropertyChangeFP, AccessBridge_PropertyChangeFP)
    CALL_SET_EVENT_FP(setFocusGainedFP, AccessBridge_FocusGainedFP)
    CALL_SET_EVENT_FP(setFocusLostFP, AccessBridge_FocusLostFP)
    CALL_SET_EVENT_FP(setCaretUpdateFP, AccessBridge_CaretUpdateFP)
    CALL_SET_EVENT_FP(setMouseClickedFP, AccessBridge_MouseClickedFP)
    CALL_SET_EVENT_FP(setMouseEnteredFP, AccessBridge_MouseEnteredFP)
    CALL_SET_EVENT_FP(setMouseExitedFP, AccessBridge_MouseExitedFP)
    CALL_SET_EVENT_FP(setMousePressedFP, AccessBridge_MousePressedFP)
    CALL_SET_EVENT_FP(setMouseReleasedFP, AccessBridge_MouseReleasedFP)
    CALL_SET_EVENT_FP(setMenuCanceledFP, AccessBridge_MenuCanceledFP)
    CALL_SET_EVENT_FP(setMenuDeselectedFP, AccessBridge_MenuDeselectedFP)
    CALL_SET_EVENT_FP(setMenuSelectedFP, AccessBridge_MenuSelectedFP)
    CALL_SET_EVENT_FP(setPopupMenuCanceledFP, AccessBridge_PopupMenuCanceledFP)
    CALL_SET_EVENT_FP(setPopupMenuWillBecomeInvisibleFP, AccessBridge_PopupMenuWillBecomeInvisibleFP)
    CALL_SET_EVENT_FP(setPopupMenuWillBecomeVisibleFP, AccessBridge_PopupMenuWillBecomeVisibleFP)

    CALL_SET_EVENT_FP(setPropertyNameChangeFP, AccessBridge_PropertyNameChangeFP)
    CALL_SET_EVENT_FP(setPropertyDescriptionChangeFP, AccessBridge_PropertyDescriptionChangeFP)
    CALL_SET_EVENT_FP(setPropertyStateChangeFP, AccessBridge_PropertyStateChangeFP)
    CALL_SET_EVENT_FP(setPropertyValueChangeFP, AccessBridge_PropertyValueChangeFP)
    CALL_SET_EVENT_FP(setPropertySelectionChangeFP, AccessBridge_PropertySelectionChangeFP)
    CALL_SET_EVENT_FP(setPropertyTextChangeFP, AccessBridge_PropertyTextChangeFP)
    CALL_SET_EVENT_FP(setPropertyCaretChangeFP, AccessBridge_PropertyCaretChangeFP)
    CALL_SET_EVENT_FP(setPropertyVisibleDataChangeFP, AccessBridge_PropertyVisibleDataChangeFP)
    CALL_SET_EVENT_FP(setPropertyChildChangeFP, AccessBridge_PropertyChildChangeFP)
    CALL_SET_EVENT_FP(setPropertyActiveDescendentChangeFP, AccessBridge_PropertyActiveDescendentChangeFP)

    CALL_SET_EVENT_FP(setPropertyTableModelChangeFP, AccessBridge_PropertyTableModelChangeFP)
