/*******************************************************************************
 *
 * Module Name: dbxface - AML Debugger external interfaces
 *
 ******************************************************************************/

/*
 * Copyright (C) 2000 - 2015, Intel Corp.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions, and the following disclaimer,
 *    without modification.
 * 2. Redistributions in binary form must reproduce at minimum a disclaimer
 *    substantially similar to the "NO WARRANTY" disclaimer below
 *    ("Disclaimer") and any redistribution must be conditioned upon
 *    including a substantially similar Disclaimer requirement for further
 *    binary redistribution.
 * 3. Neither the names of the above-listed copyright holders nor the names
 *    of any contributors may be used to endorse or promote products derived
 *    from this software without specific prior written permission.
 *
 * Alternatively, this software may be distributed under the terms of the
 * GNU General Public License ("GPL") version 2 as published by the Free
 * Software Foundation.
 *
 * NO WARRANTY
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTIBILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * HOLDERS OR CONTRIBUTORS BE LIABLE FOR SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
 * STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING
 * IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGES.
 */

#include "acpi.h"
#include "accommon.h"
#include "amlcode.h"
#include "acdebug.h"

#ifdef ACPI_DEBUGGER

#define _COMPONENT          ACPI_CA_DEBUGGER
        ACPI_MODULE_NAME    ("dbxface")


/* Local prototypes */

static ACPI_STATUS
AcpiDbStartCommand (
    ACPI_WALK_STATE         *WalkState,
    ACPI_PARSE_OBJECT       *Op);

#ifdef ACPI_OBSOLETE_FUNCTIONS
void
AcpiDbMethodEnd (
    ACPI_WALK_STATE         *WalkState);
#endif


/*******************************************************************************
 *
 * FUNCTION:    AcpiDbStartCommand
 *
 * PARAMETERS:  WalkState       - Current walk
 *              Op              - Current executing Op, from AML interpreter
 *
 * RETURN:      Status
 *
 * DESCRIPTION: Enter debugger command loop
 *
 ******************************************************************************/

static ACPI_STATUS
AcpiDbStartCommand (
    ACPI_WALK_STATE         *WalkState,
    ACPI_PARSE_OBJECT       *Op)
{
    ACPI_STATUS             Status;


    /* TBD: [Investigate] are there namespace locking issues here? */

    /* AcpiUtReleaseMutex (ACPI_MTX_NAMESPACE); */

    /* Go into the command loop and await next user command */


    AcpiGbl_MethodExecuting = TRUE;
    Status = AE_CTRL_TRUE;
    while (Status == AE_CTRL_TRUE)
    {
        if (AcpiGbl_DebuggerConfiguration == DEBUGGER_MULTI_THREADED)
        {
            /* Handshake with the front-end that gets user command lines */

            AcpiOsReleaseMutex (AcpiGbl_DbCommandComplete);

            Status = AcpiOsAcquireMutex (AcpiGbl_DbCommandReady,
                ACPI_WAIT_FOREVER);
            if (ACPI_FAILURE (Status))
            {
                return (Status);
            }
        }
        else
        {
            /* Single threaded, we must get a command line ourselves */

            /* Force output to console until a command is entered */

            AcpiDbSetOutputDestination (ACPI_DB_CONSOLE_OUTPUT);

            /* Different prompt if method is executing */

            if (!AcpiGbl_MethodExecuting)
            {
                AcpiOsPrintf ("%1c ", ACPI_DEBUGGER_COMMAND_PROMPT);
            }
            else
            {
                AcpiOsPrintf ("%1c ", ACPI_DEBUGGER_EXECUTE_PROMPT);
            }

            /* Get the user input line */

            Status = AcpiOsGetLine (AcpiGbl_DbLineBuf,
                ACPI_DB_LINE_BUFFER_SIZE, NULL);
            if (ACPI_FAILURE (Status))
            {
                ACPI_EXCEPTION ((AE_INFO, Status,
                    "While parsing command line"));
                return (Status);
            }
        }

        Status = AcpiDbCommandDispatch (AcpiGbl_DbLineBuf, WalkState, Op);
    }

    /* AcpiUtAcquireMutex (ACPI_MTX_NAMESPACE); */

    return (Status);
}


/*******************************************************************************
 *
 * FUNCTION:    AcpiDbSignalBreakPoint
 *
 * PARAMETERS:  WalkState       - Current walk
 *
 * RETURN:      Status
 *
 * DESCRIPTION: Called for AML_BREAK_POINT_OP
 *
 ******************************************************************************/

void
AcpiDbSignalBreakPoint (
    ACPI_WALK_STATE         *WalkState)
{

#ifndef ACPI_APPLICATION
    if (AcpiGbl_DbThreadId != AcpiOsGetThreadId ())
    {
        return;
    }
#endif

    /*
     * Set the single-step flag. This will cause the debugger (if present)
     * to break to the console within the AML debugger at the start of the
     * next AML instruction.
     */
    AcpiGbl_CmSingleStep = TRUE;
    AcpiOsPrintf ("**break** Executed AML BreakPoint opcode\n");
}


/*******************************************************************************
 *
 * FUNCTION:    AcpiDbSingleStep
 *
 * PARAMETERS:  WalkState       - Current walk
 *              Op              - Current executing op (from aml interpreter)
 *              OpcodeClass     - Class of the current AML Opcode
 *
 * RETURN:      Status
 *
 * DESCRIPTION: Called just before execution of an AML opcode.
 *
 ******************************************************************************/

ACPI_STATUS
AcpiDbSingleStep (
    ACPI_WALK_STATE         *WalkState,
    ACPI_PARSE_OBJECT       *Op,
    UINT32                  OpcodeClass)
{
    ACPI_PARSE_OBJECT       *Next;
    ACPI_STATUS             Status = AE_OK;
    UINT32                  OriginalDebugLevel;
    ACPI_PARSE_OBJECT       *DisplayOp;
    ACPI_PARSE_OBJECT       *ParentOp;
    UINT32                  AmlOffset;


    ACPI_FUNCTION_ENTRY ();


#ifndef ACPI_APPLICATION
    if (AcpiGbl_DbThreadId != AcpiOsGetThreadId ())
    {
        return (AE_OK);
    }
#endif

    /* Check the abort flag */

    if (AcpiGbl_AbortMethod)
    {
        AcpiGbl_AbortMethod = FALSE;
        return (AE_ABORT_METHOD);
    }

    AmlOffset = (UINT32) ACPI_PTR_DIFF (Op->Common.Aml,
        WalkState->ParserState.AmlStart);

    /* Check for single-step breakpoint */

    if (WalkState->MethodBreakpoint &&
       (WalkState->MethodBreakpoint <= AmlOffset))
    {
        /* Check if the breakpoint has been reached or passed */
        /* Hit the breakpoint, resume single step, reset breakpoint */

        AcpiOsPrintf ("***Break*** at AML offset %X\n", AmlOffset);
        AcpiGbl_CmSingleStep = TRUE;
        AcpiGbl_StepToNextCall = FALSE;
        WalkState->MethodBreakpoint = 0;
    }

    /* Check for user breakpoint (Must be on exact Aml offset) */

    else if (WalkState->UserBreakpoint &&
            (WalkState->UserBreakpoint == AmlOffset))
    {
        AcpiOsPrintf ("***UserBreakpoint*** at AML offset %X\n",
            AmlOffset);
        AcpiGbl_CmSingleStep = TRUE;
        AcpiGbl_StepToNextCall = FALSE;
        WalkState->MethodBreakpoint = 0;
    }

    /*
     * Check if this is an opcode that we are interested in --
     * namely, opcodes that have arguments
     */
    if (Op->Common.AmlOpcode == AML_INT_NAMEDFIELD_OP)
    {
        return (AE_OK);
    }

    switch (OpcodeClass)
    {
    case AML_CLASS_UNKNOWN:
    case AML_CLASS_ARGUMENT:    /* constants, literals, etc. do nothing */

        return (AE_OK);

    default:

        /* All other opcodes -- continue */
        break;
    }

    /*
     * Under certain debug conditions, display this opcode and its operands
     */
    if ((AcpiGbl_DbOutputToFile)            ||
        (AcpiGbl_CmSingleStep)              ||
        (AcpiDbgLevel & ACPI_LV_PARSE))
    {
        if ((AcpiGbl_DbOutputToFile)        ||
            (AcpiDbgLevel & ACPI_LV_PARSE))
        {
            AcpiOsPrintf ("\n[AmlDebug] Next AML Opcode to execute:\n");
        }

        /*
         * Display this op (and only this op - zero out the NEXT field
         * temporarily, and disable parser trace output for the duration of
         * the display because we don't want the extraneous debug output)
         */
        OriginalDebugLevel = AcpiDbgLevel;
        AcpiDbgLevel &= ~(ACPI_LV_PARSE | ACPI_LV_FUNCTIONS);
        Next = Op->Common.Next;
        Op->Common.Next = NULL;


        DisplayOp = Op;
        ParentOp = Op->Common.Parent;
        if (ParentOp)
        {
            if ((WalkState->ControlState) &&
                (WalkState->ControlState->Common.State ==
                    ACPI_CONTROL_PREDICATE_EXECUTING))
            {
                /*
                 * We are executing the predicate of an IF or WHILE statement
                 * Search upwards for the containing IF or WHILE so that the
                 * entire predicate can be displayed.
                 */
                while (ParentOp)
                {
                    if ((ParentOp->Common.AmlOpcode == AML_IF_OP) ||
                        (ParentOp->Common.AmlOpcode == AML_WHILE_OP))
                    {
                        DisplayOp = ParentOp;
                        break;
                    }
                    ParentOp = ParentOp->Common.Parent;
                }
            }
            else
            {
                while (ParentOp)
                {
                    if ((ParentOp->Common.AmlOpcode == AML_IF_OP)     ||
                        (ParentOp->Common.AmlOpcode == AML_ELSE_OP)   ||
                        (ParentOp->Common.AmlOpcode == AML_SCOPE_OP)  ||
                        (ParentOp->Common.AmlOpcode == AML_METHOD_OP) ||
                        (ParentOp->Common.AmlOpcode == AML_WHILE_OP))
                    {
                        break;
                    }
                    DisplayOp = ParentOp;
                    ParentOp = ParentOp->Common.Parent;
                }
            }
        }

        /* Now we can display it */

#ifdef ACPI_DISASSEMBLER
        AcpiDmDisassemble (WalkState, DisplayOp, ACPI_UINT32_MAX);
#endif

        if ((Op->Common.AmlOpcode == AML_IF_OP) ||
            (Op->Common.AmlOpcode == AML_WHILE_OP))
        {
            if (WalkState->ControlState->Common.Value)
            {
                AcpiOsPrintf ("Predicate = [True], IF block was executed\n");
            }
            else
            {
                AcpiOsPrintf ("Predicate = [False], Skipping IF block\n");
            }
        }
        else if (Op->Common.AmlOpcode == AML_ELSE_OP)
        {
            AcpiOsPrintf ("Predicate = [False], ELSE block was executed\n");
        }

        /* Restore everything */

        Op->Common.Next = Next;
        AcpiOsPrintf ("\n");
        if ((AcpiGbl_DbOutputToFile)        ||
            (AcpiDbgLevel & ACPI_LV_PARSE))
        {
            AcpiOsPrintf ("\n");
        }
        AcpiDbgLevel = OriginalDebugLevel;
    }

    /* If we are not single stepping, just continue executing the method */

    if (!AcpiGbl_CmSingleStep)
    {
        return (AE_OK);
    }

    /*
     * If we are executing a step-to-call command,
     * Check if this is a method call.
     */
    if (AcpiGbl_StepToNextCall)
    {
        if (Op->Common.AmlOpcode != AML_INT_METHODCALL_OP)
        {
            /* Not a method call, just keep executing */

            return (AE_OK);
        }

        /* Found a method call, stop executing */

        AcpiGbl_StepToNextCall = FALSE;
    }

    /*
     * If the next opcode is a method call, we will "step over" it
     * by default.
     */
    if (Op->Common.AmlOpcode == AML_INT_METHODCALL_OP)
    {
        /* Force no more single stepping while executing called method */

        AcpiGbl_CmSingleStep = FALSE;

        /*
         * Set the breakpoint on/before the call, it will stop execution
         * as soon as we return
         */
        WalkState->MethodBreakpoint = 1;  /* Must be non-zero! */
    }


    Status = AcpiDbStartCommand (WalkState, Op);

    /* User commands complete, continue execution of the interrupted method */

    return (Status);
}


/*******************************************************************************
 *
 * FUNCTION:    AcpiInitializeDebugger
 *
 * PARAMETERS:  None
 *
 * RETURN:      Status
 *
 * DESCRIPTION: Init and start debugger
 *
 ******************************************************************************/

ACPI_STATUS
AcpiInitializeDebugger (
    void)
{
    ACPI_STATUS             Status;


    ACPI_FUNCTION_TRACE (AcpiInitializeDebugger);


    /* Init globals */

    AcpiGbl_DbBuffer            = NULL;
    AcpiGbl_DbFilename          = NULL;
    AcpiGbl_DbOutputToFile      = FALSE;

    AcpiGbl_DbDebugLevel        = ACPI_LV_VERBOSITY2;
    AcpiGbl_DbConsoleDebugLevel = ACPI_NORMAL_DEFAULT | ACPI_LV_TABLES;
    AcpiGbl_DbOutputFlags       = ACPI_DB_CONSOLE_OUTPUT;

    AcpiGbl_DbOpt_NoIniMethods  = FALSE;

    AcpiGbl_DbBuffer = AcpiOsAllocate (ACPI_DEBUG_BUFFER_SIZE);
    if (!AcpiGbl_DbBuffer)
    {
        return_ACPI_STATUS (AE_NO_MEMORY);
    }
    memset (AcpiGbl_DbBuffer, 0, ACPI_DEBUG_BUFFER_SIZE);

    /* Initial scope is the root */

    AcpiGbl_DbScopeBuf [0] = AML_ROOT_PREFIX;
    AcpiGbl_DbScopeBuf [1] =  0;
    AcpiGbl_DbScopeNode = AcpiGbl_RootNode;

    /* Initialize user commands loop */

    AcpiGbl_DbTerminateLoop = FALSE;

    /*
     * If configured for multi-thread support, the debug executor runs in
     * a separate thread so that the front end can be in another address
     * space, environment, or even another machine.
     */
    if (AcpiGbl_DebuggerConfiguration & DEBUGGER_MULTI_THREADED)
    {
        /* These were created with one unit, grab it */

        Status = AcpiOsAcquireMutex (AcpiGbl_DbCommandComplete,
            ACPI_WAIT_FOREVER);
        if (ACPI_FAILURE (Status))
        {
            AcpiOsPrintf ("Could not get debugger mutex\n");
            return_ACPI_STATUS (Status);
        }

        Status = AcpiOsAcquireMutex (AcpiGbl_DbCommandReady,
            ACPI_WAIT_FOREVER);
        if (ACPI_FAILURE (Status))
        {
            AcpiOsPrintf ("Could not get debugger mutex\n");
            return_ACPI_STATUS (Status);
        }

        /* Create the debug execution thread to execute commands */

        AcpiGbl_DbThreadsTerminated = FALSE;
        Status = AcpiOsExecute (OSL_DEBUGGER_MAIN_THREAD,
            AcpiDbExecuteThread, NULL);
        if (ACPI_FAILURE (Status))
        {
            ACPI_EXCEPTION ((AE_INFO, Status,
                "Could not start debugger thread"));
            AcpiGbl_DbThreadsTerminated = TRUE;
            return_ACPI_STATUS (Status);
        }
    }
    else
    {
        AcpiGbl_DbThreadId = AcpiOsGetThreadId ();
    }

    return_ACPI_STATUS (AE_OK);
}

ACPI_EXPORT_SYMBOL (AcpiInitializeDebugger)


/*******************************************************************************
 *
 * FUNCTION:    AcpiTerminateDebugger
 *
 * PARAMETERS:  None
 *
 * RETURN:      None
 *
 * DESCRIPTION: Stop debugger
 *
 ******************************************************************************/

void
AcpiTerminateDebugger (
    void)
{

    /* Terminate the AML Debugger */

    AcpiGbl_DbTerminateLoop = TRUE;

    if (AcpiGbl_DebuggerConfiguration & DEBUGGER_MULTI_THREADED)
    {
        AcpiOsReleaseMutex (AcpiGbl_DbCommandReady);

        /* Wait the AML Debugger threads */

        while (!AcpiGbl_DbThreadsTerminated)
        {
            AcpiOsSleep (100);
        }
    }

    if (AcpiGbl_DbBuffer)
    {
        AcpiOsFree (AcpiGbl_DbBuffer);
        AcpiGbl_DbBuffer = NULL;
    }

    /* Ensure that debug output is now disabled */

    AcpiGbl_DbOutputFlags = ACPI_DB_DISABLE_OUTPUT;
}

ACPI_EXPORT_SYMBOL (AcpiTerminateDebugger)


/*******************************************************************************
 *
 * FUNCTION:    AcpiSetDebuggerThreadId
 *
 * PARAMETERS:  ThreadId        - Debugger thread ID
 *
 * RETURN:      None
 *
 * DESCRIPTION: Set debugger thread ID
 *
 ******************************************************************************/

void
AcpiSetDebuggerThreadId (
    ACPI_THREAD_ID          ThreadId)
{
    AcpiGbl_DbThreadId = ThreadId;
}

ACPI_EXPORT_SYMBOL (AcpiSetDebuggerThreadId)

#endif /* ACPI_DEBUGGER */
