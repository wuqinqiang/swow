/*
  +--------------------------------------------------------------------------+
  | Swow                                                                     |
  +--------------------------------------------------------------------------+
  | Licensed under the Apache License, Version 2.0 (the "License");          |
  | you may not use this file except in compliance with the License.         |
  | You may obtain a copy of the License at                                  |
  | http://www.apache.org/licenses/LICENSE-2.0                               |
  | Unless required by applicable law or agreed to in writing, software      |
  | distributed under the License is distributed on an "AS IS" BASIS,        |
  | WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. |
  | See the License for the specific language governing permissions and      |
  | limitations under the License. See accompanying LICENSE file.            |
  +--------------------------------------------------------------------------+
  | Author: Twosee <twose@qq.com>                                            |
  +--------------------------------------------------------------------------+
 */

#include "swow_coroutine.h"

#include "swow_debug.h"

#if SWOW_COROUTINE_SWAP_SILENCE_CONTEXT
#define E_SILENCE_MAGIC (1 << 31)
#endif

SWOW_API zend_class_entry *swow_coroutine_ce;
SWOW_API zend_object_handlers swow_coroutine_handlers;

SWOW_API zend_class_entry *swow_coroutine_exception_ce;
SWOW_API zend_class_entry *swow_coroutine_cross_exception_ce;
SWOW_API zend_class_entry *swow_coroutine_term_exception_ce;
SWOW_API zend_class_entry *swow_coroutine_kill_exception_ce;

SWOW_API CAT_GLOBALS_DECLARE(swow_coroutine)

CAT_GLOBALS_CTOR_DECLARE_SZ(swow_coroutine)

/* pre declare */
static cat_bool_t swow_coroutine_construct(swow_coroutine_t *scoroutine, zval *zcallable, size_t stack_page_size, size_t c_stack_size);
static void swow_coroutine_close(swow_coroutine_t *scoroutine);
static void swow_coroutine_handle_cross_exception(zend_object *cross_exception);
static void swow_coroutine_handle_not_null_zdata(swow_coroutine_t *sscoroutine, swow_coroutine_t *rscoroutine, zval **zdata_ptr, const cat_bool_t handle_ref);
static cat_data_t *swow_coroutine_resume_deny(cat_coroutine_t *coroutine, cat_data_t *data);

static cat_always_inline size_t swow_coroutine_align_stack_page_size(size_t size)
{
    if (size == 0) {
        size = SWOW_COROUTINE_G(default_stack_page_size);
    } else if (UNEXPECTED(size < CAT_COROUTINE_MIN_STACK_SIZE)) {
        size = SWOW_COROUTINE_MIN_STACK_PAGE_SIZE;
    } else if (UNEXPECTED(size > CAT_COROUTINE_MAX_STACK_SIZE)) {
        size = SWOW_COROUTINE_MAX_STACK_PAGE_SIZE;
    } else {
        size = CAT_MEMORY_ALIGNED_SIZE_EX(size, SWOW_COROUTINE_STACK_PAGE_ALIGNED_SIZE);
    }

    return size;
}

static zend_object *swow_coroutine_create_object(zend_class_entry *ce)
{
    swow_coroutine_t *scoroutine = swow_object_alloc(swow_coroutine_t, ce, swow_coroutine_handlers);

    cat_coroutine_init(&scoroutine->coroutine);

    return &scoroutine->std;
}

static void swow_coroutine_dtor_object(zend_object *object)
{
    /* try to call __destruct first */
    zend_objects_destroy_object(object);

    /* force kill the coroutine */
    swow_coroutine_t *scoroutine = swow_coroutine_get_from_object(object);

    /* we should never kill the main coroutine */
    if (UNEXPECTED(scoroutine == swow_coroutine_get_main())) {
        return;
    }

    while (UNEXPECTED(swow_coroutine_is_alive(scoroutine))) {
        /* not finished, should be discard */
        if (UNEXPECTED(!swow_coroutine_kill(scoroutine, NULL, ~0))) {
            cat_core_error(COROUTINE, "Kill coroutine failed when destruct object, reason: %s", cat_get_last_error_message());
        }
    }
}

static void swow_coroutine_free_object(zend_object *object)
{
    swow_coroutine_t *scoroutine = swow_coroutine_get_from_object(object);

    if (UNEXPECTED(swow_coroutine_is_available(scoroutine))) {
        /* created but never run (or it is main coroutine) */
        swow_coroutine_close(scoroutine);
    }

    zend_object_std_dtor(&scoroutine->std);
}

static void swow_coroutine_function_handle_exception(void)
{
    CAT_ASSERT(EG(exception) != NULL);

    zend_exception_restore();

    /* keep slient for killer */
    if (instanceof_function(EG(exception)->ce, swow_coroutine_kill_exception_ce)) {
        OBJ_RELEASE(EG(exception));
        EG(exception) = NULL;
        return;
    }

    if (Z_TYPE(EG(user_exception_handler)) != IS_UNDEF) {
        zval origin_user_exception_handler;
        zval param, retval;
        zend_object *old_exception;
        old_exception = EG(exception);
        EG(exception) = NULL;
        ZVAL_OBJ(&param, old_exception);
        ZVAL_COPY_VALUE(&origin_user_exception_handler, &EG(user_exception_handler));
        if (call_user_function(CG(function_table), NULL, &origin_user_exception_handler, &retval, 1, &param) == SUCCESS) {
            zval_ptr_dtor(&retval);
            if (EG(exception)) {
                if (EG(exception) == old_exception) {
                    GC_DELREF(old_exception);
                } else {
                    zend_exception_set_previous(EG(exception), old_exception);
                }
            }
        }
        if (EG(exception) == NULL) {
            EG(exception) = old_exception;
        }
    }

    if (EG(exception)) {
        zend_long severity = SWOW_COROUTINE_G(exception_error_severity);
        if (severity > E_NONE) {
            zend_exception_error(EG(exception), severity);
        } else {
            OBJ_RELEASE(EG(exception));
            EG(exception) = NULL;
        }
    }
}

static zval *swow_coroutine_function(zval *zdata)
{
    static const zend_execute_data dummy_execute_data;
    swow_coroutine_t *scoroutine = swow_coroutine_get_current();
    swow_coroutine_exector_t *executor = scoroutine->executor;
    zval zcallable = executor->zcallable;
    zend_fcall_info fci;
    zval retval;

    CAT_ASSERT(executor != NULL);

    /* add to scoroutines map (we can not add beofre run otherwise refcount would never be 0) */
    do {
        zval ztmp;
        ZVAL_OBJ(&ztmp, &scoroutine->std);
        zend_hash_index_update(SWOW_COROUTINE_G(map), scoroutine->coroutine.id, &ztmp);
        GC_ADDREF(&scoroutine->std);
    } while (0);

    /* prepare function call info */
    fci.size = sizeof(fci);
    ZVAL_UNDEF(&fci.function_name);
    fci.object = NULL;
    /* params will be copied by zend_call_function */
    if (likely(zdata == SWOW_COROUTINE_DATA_NULL)) {
        fci.param_count = 0;
    } else if (Z_TYPE_P(zdata) != IS_PTR) {
        Z_TRY_DELREF_P(zdata);
        fci.param_count = 1;
        fci.params = zdata;
    } else {
        zend_fcall_info *fci_ptr = (zend_fcall_info *) Z_PTR_P(zdata);
        fci.param_count = fci_ptr->param_count;
        fci.params = fci_ptr->params;
    }
    fci.no_separation = 0;
    fci.retval = &retval;

    /* call function */
    EG(current_execute_data) = (zend_execute_data *) &dummy_execute_data;
    (void) zend_call_function(&fci, &executor->fcc);
    EG(current_execute_data) = NULL;
    scoroutine->coroutine.flags |= SWOW_COROUTINE_FLAG_MAIN_FINISHED;
    if (UNEXPECTED(EG(exception) != NULL)) {
        swow_coroutine_function_handle_exception();
    }

    /* discard all possible resources (varibles by "use" in zend_closure) */
    EG(current_execute_data) = (zend_execute_data *) &dummy_execute_data;
    ZVAL_NULL(&executor->zcallable);
    zval_ptr_dtor(&zcallable);
    EG(current_execute_data) = NULL;
    if (UNEXPECTED(EG(exception) != NULL)) {
        swow_coroutine_function_handle_exception();
    }
    scoroutine->coroutine.flags |= SWOW_COROUTINE_FLAG_ALL_FINISHED;

    /* ob end clean */
#if SWOW_COROUTINE_SWAP_OUTPUT_GLOBALS
    swow_output_globals_fast_end();
#endif

    swow_coroutine_t *previous_scoroutine = swow_coroutine_get_previous(scoroutine);
    CAT_ASSERT(previous_scoroutine != NULL);
    /* break releation with origin */
    GC_DELREF(&previous_scoroutine->std);
    /* swap to origin */
    swow_coroutine_executor_switch(previous_scoroutine);
    /* solve retval */
    if (Z_TYPE_P(fci.retval) == IS_UNDEF || Z_TYPE_P(fci.retval) == IS_NULL) {
        return SWOW_COROUTINE_DATA_NULL;
    } else {
        scoroutine->coroutine.opcodes |= SWOW_COROUTINE_OPCODE_ACCEPT_ZDATA;
        swow_coroutine_handle_not_null_zdata(scoroutine, previous_scoroutine, &fci.retval, cat_true);
        return fci.retval;
    }
}

#ifdef SWOW_COROUTINE_ENABLE_CUSTOM_ENTRY
static swow_coroutine_t *swow_coroutine_create_custom_object(zval *zcallable)
{
    zend_class_entry *custom_entry = SWOW_COROUTINE_G(custom_entry);
    swow_coroutine_t *scoroutine;
    zval zscoroutine;

    scoroutine = swow_coroutine_get_from_object(swow_object_create(custom_entry));
    ZVAL_OBJ(&zscoroutine, &scoroutine->std);
    swow_call_method_with_1_params(
        &zscoroutine,
        custom_entry,
        &custom_entry->constructor,
        "__construct",
        NULL,
        zcallable
    );
    if (UNEXPECTED(EG(exception))) {
        cat_update_last_error_ez("Exception occurred during construction");
        zend_object_release(&scoroutine->std);
        return NULL;
    }

    return scoroutine;
}
#endif

SWOW_API swow_coroutine_t *swow_coroutine_create(zval *zcallable)
{
    return swow_coroutine_create_ex(zcallable, 0, 0);
}

SWOW_API swow_coroutine_t *swow_coroutine_create_ex(zval *zcallable, size_t stack_page_size, size_t c_stack_size)
{
    swow_coroutine_t *scoroutine;

#ifdef SWOW_COROUTINE_ENABLE_CUSTOM_ENTRY
    if (UNEXPECTED(SWOW_COROUTINE_G(custom_entry) != NULL)) {
        return swow_coroutine_create_custom_object(zcallable);
    }
#endif
    scoroutine = swow_coroutine_get_from_object(
        swow_object_create(swow_coroutine_ce)
    );
    if (UNEXPECTED(!swow_coroutine_construct(scoroutine, zcallable, stack_page_size, c_stack_size))) {
        zend_object_release(&scoroutine->std);
        return NULL;
    }

    return scoroutine;
}

static cat_bool_t swow_coroutine_construct(swow_coroutine_t *scoroutine, zval *zcallable, size_t stack_page_size, size_t c_stack_size)
{
    /* check arguments */
    zend_fcall_info_cache fcc;
    do {
        char *error;
        if (!zend_is_callable_ex(zcallable, NULL, 0, NULL, &fcc, &error)) {
            cat_update_last_error(CAT_EMISUSE, "Coroutine function must be callable, %s", error);
            efree(error);
            return cat_false;
        }
        efree(error);
    } while (0);

    /* create C coroutine */
    cat_coroutine_t *coroutine = cat_coroutine_create_ex(
        &scoroutine->coroutine,
        (cat_coroutine_function_t) swow_coroutine_function,
        c_stack_size
    );
    if (UNEXPECTED(coroutine == NULL)) {
        return cat_false;
    }
    coroutine->opcodes |= SWOW_COROUTINE_OPCODE_ACCEPT_ZDATA;

    /* align stack page size */
    stack_page_size = swow_coroutine_align_stack_page_size(stack_page_size);
    /* alloc vm stack memory */
    zend_vm_stack vm_stack = (zend_vm_stack) emalloc(stack_page_size);
    /* assign the end to executor */
    swow_coroutine_exector_t *executor = (swow_coroutine_exector_t *) ZEND_VM_STACK_ELEMENTS(vm_stack);
    /* init executor */
    do {
        /* init exector */
        executor->bailout = NULL;
        executor->vm_stack = vm_stack;
        executor->vm_stack->top = (zval *) (((char *) executor) + CAT_MEMORY_ALIGNED_SIZE_EX(sizeof(*executor), sizeof(zval)));
        executor->vm_stack->end = (zval *) (((char *) vm_stack) + stack_page_size);
        executor->vm_stack->prev = NULL;
        executor->vm_stack_top = executor->vm_stack->top;
        executor->vm_stack_end = executor->vm_stack->end;
#if PHP_VERSION_ID >= 70300
        executor->vm_stack_page_size = stack_page_size;
#endif
        executor->current_execute_data = NULL;
        executor->exception = NULL;
#if SWOW_COROUTINE_SWAP_INTERNAL_CONTEXT
        executor->error_handling = EH_NORMAL;
#endif
#if SWOW_COROUTINE_SWAP_BASIC_GLOBALS
        executor->array_walk_context = NULL;
#endif
#if SWOW_COROUTINE_SWAP_OUTPUT_GLOBALS
        executor->output_globals = NULL;
#endif
#if SWOW_COROUTINE_SWAP_SILENCE_CONTEXT
        executor->error_reporting_for_silence = E_SILENCE_MAGIC;
#endif
        /* save function cache */
        ZVAL_COPY(&executor->zcallable, zcallable);
        executor->fcc = fcc;

        /* it's unnecessary to init the zdata */
        /* ZVAL_UNDEF(&executor->zdata); */
        executor->cross_exception = NULL;
    } while (0);
    /* executor ok */
    scoroutine->executor = executor;

    return cat_true;
}

static void swow_coroutine_close(swow_coroutine_t *scoroutine)
{
    swow_coroutine_exector_t *executor = scoroutine->executor;

    CAT_ASSERT(executor != NULL);

    /* we release the context resources here which are created during the runtime  */
#if SWOW_COROUTINE_SWAP_OUTPUT_GLOBALS
    if (UNEXPECTED(executor->output_globals != NULL)) {
        efree(executor->output_globals);
    }
#endif
#if SWOW_COROUTINE_SWAP_BASIC_GLOBALS
    if (UNEXPECTED(executor->array_walk_context != NULL)) {
        efree(executor->array_walk_context);
    }
#endif

    /* discard function (usually cleaned up before the coroutine finished, unless the coroutine never run) */
    if (UNEXPECTED(!ZVAL_IS_NULL(&executor->zcallable))) {
        zval_ptr_dtor(&executor->zcallable);
    }

    /* free zend vm stack */
    if (EXPECTED(executor->vm_stack != NULL)) {
        zend_vm_stack stack = executor->vm_stack;
        do {
            zend_vm_stack prev = stack->prev;
            efree(stack);
            stack = prev;
        } while (stack);
    } else {
        efree(executor);
    }

    scoroutine->executor = NULL;
}

SWOW_API void swow_coroutine_executor_switch(swow_coroutine_t *scoroutine)
{
    swow_coroutine_executor_save(swow_coroutine_get_current()->executor);
    swow_coroutine_executor_recover(scoroutine->executor);
}

SWOW_API void swow_coroutine_executor_save(swow_coroutine_exector_t *executor)
{
    zend_executor_globals *eg = SWOW_GLOBALS_FAST_PTR(executor_globals);
    executor->bailout = eg->bailout;
    executor->vm_stack_top = eg->vm_stack_top;
    executor->vm_stack_end = eg->vm_stack_end;
    executor->vm_stack = eg->vm_stack;
#if PHP_VERSION_ID >= 70300
    executor->vm_stack_page_size = eg->vm_stack_page_size;
#endif
    executor->current_execute_data = eg->current_execute_data;
    executor->exception = eg->exception;
#if SWOW_COROUTINE_SWAP_INTERNAL_CONTEXT
    executor->error_handling = eg->error_handling;
#endif
#if SWOW_COROUTINE_SWAP_BASIC_GLOBALS
    do {
        swow_fcall_t *fcall = (swow_fcall_t *) &BG(array_walk_fci);
        if (UNEXPECTED(fcall->info.size != 0)) {
            if (UNEXPECTED(executor->array_walk_context == NULL)) {
                executor->array_walk_context = (swow_fcall_t *) emalloc(sizeof(*fcall));
            }
            memcpy(executor->array_walk_context, fcall, sizeof(*fcall));
            memset(fcall, 0, sizeof(*fcall));
        }
    } while (0);
#endif
#if SWOW_COROUTINE_SWAP_OUTPUT_GLOBALS
    do {
        zend_output_globals *og = SWOW_GLOBALS_PTR(output_globals);
        if (UNEXPECTED(og->handlers.elements != NULL)) {
            if (UNEXPECTED(executor->output_globals == NULL)) {
                executor->output_globals = (zend_output_globals *) emalloc(sizeof(zend_output_globals));
            }
            memcpy(executor->output_globals, og, sizeof(zend_output_globals));
            php_output_activate();
        }
    } while (0);
#endif
#if SWOW_COROUTINE_SWAP_SILENCE_CONTEXT
    do {
        if (UNEXPECTED(executor->error_reporting_for_silence != E_SILENCE_MAGIC)) {
            int error_reporting = EG(error_reporting);
            executor->error_reporting_for_silence = EG(error_reporting);
            EG(error_reporting) = error_reporting;
        }
    } while (0);
#endif
}

SWOW_API void swow_coroutine_executor_recover(swow_coroutine_exector_t *executor)
{
    zend_executor_globals *eg = SWOW_GLOBALS_FAST_PTR(executor_globals);
    eg->bailout = executor->bailout;
    eg->vm_stack_top = executor->vm_stack_top;
    eg->vm_stack_end = executor->vm_stack_end;
    eg->vm_stack = executor->vm_stack;
#if PHP_VERSION_ID >= 70300
    eg->vm_stack_page_size = executor->vm_stack_page_size;
#endif
    eg->current_execute_data = executor->current_execute_data;
    eg->exception = executor->exception;
#if SWOW_COROUTINE_SWAP_INTERNAL_CONTEXT
    eg->error_handling = executor->error_handling;
#endif
#if SWOW_COROUTINE_SWAP_BASIC_GLOBALS
    do {
        swow_fcall_t *fcall = executor->array_walk_context;
        if (UNEXPECTED(fcall != NULL && fcall->info.size != 0)) {
            memcpy(&BG(array_walk_fci), fcall, sizeof(*fcall));
            fcall->info.size = 0;
        }
    } while (0);
#endif
#if SWOW_COROUTINE_SWAP_OUTPUT_GLOBALS
    do {
        zend_output_globals *og = executor->output_globals;
        if (UNEXPECTED(og != NULL)) {
            if (UNEXPECTED(og->handlers.elements != NULL)) {
                memcpy(SWOW_GLOBALS_PTR(output_globals), og, sizeof(zend_output_globals));
                og->handlers.elements = NULL;
            }
        }
    } while (0);
#endif
#if SWOW_COROUTINE_SWAP_SILENCE_CONTEXT
    do {
        if (UNEXPECTED(executor->error_reporting_for_silence != E_SILENCE_MAGIC)) {
            int error_reporting = EG(error_reporting);
            EG(error_reporting) = executor->error_reporting_for_silence;
            executor->error_reporting_for_silence = error_reporting;
        }
    } while (0);
#endif
}

static void swow_coroutine_handle_not_null_zdata(swow_coroutine_t *sscoroutine, swow_coroutine_t *rscoroutine, zval **zdata_ptr, const cat_bool_t handle_ref)
{
    if (!(sscoroutine->coroutine.opcodes & SWOW_COROUTINE_OPCODE_ACCEPT_ZDATA)) {
        if (UNEXPECTED(rscoroutine->coroutine.opcodes & SWOW_COROUTINE_OPCODE_ACCEPT_ZDATA)) {
            cat_core_error(COROUTINE, "Internal logic error: sent unrecognized data to PHP layer");
        } else {
            /* internal raw data to internal operation coroutine */
        }
    } else {
        zval *zdata = *zdata_ptr;
        if (!(rscoroutine->coroutine.opcodes & SWOW_COROUTINE_OPCODE_ACCEPT_ZDATA)) {
            CAT_ASSERT(Z_TYPE_P(zdata) != IS_PTR);
            /* the PHP layer can not send data to the internal-controlled coroutine */
            if (handle_ref) {
                zval_ptr_dtor(zdata);
            }
            *zdata_ptr = SWOW_COROUTINE_DATA_NULL;
            return;
        } else {
            CAT_ASSERT(rscoroutine->executor != NULL);
            if (UNEXPECTED(Z_TYPE_P(zdata) == IS_PTR)) {
                /* params will be copied by zend_call_function */
                return;
            } else {
#ifdef CAT_DO_NOT_OPTIMIZE
                /* make sure the memory space of zdata is safe */
                zval *safe_zdata = &rscoroutine->executor->zdata;
                if (!handle_ref) {
                    ZVAL_COPY(safe_zdata, zdata);
                } else {
                    ZVAL_COPY_VALUE(safe_zdata, zdata);
                }
                *zdata_ptr = safe_zdata;
#else
                if (!handle_ref) {
                    /* send without copy value */
                    Z_TRY_ADDREF_P(zdata);
                } else {
                    /* make sure the memory space of zdata is safe */
                    zval *safe_zdata = &rscoroutine->executor->zdata;
                    ZVAL_COPY_VALUE(safe_zdata, zdata);
                    *zdata_ptr = safe_zdata;
                }
#endif
            }
        }
    }
}

SWOW_API cat_bool_t swow_coroutine_jump_precheck(swow_coroutine_t *scoroutine, const zval *zdata)
{
    return cat_coroutine_jump_precheck(&scoroutine->coroutine, zdata);
}

SWOW_API zval *swow_coroutine_jump(swow_coroutine_t *scoroutine, zval *zdata)
{
    swow_coroutine_t *current_scoroutine = swow_coroutine_get_current();

    /* solve origin's refcount */
    do {
        swow_coroutine_t *current_previous_scoroutine = swow_coroutine_get_previous(current_scoroutine);
        if (current_previous_scoroutine == scoroutine) {
            /* if it is yield, break the origin */
            GC_DELREF(&current_previous_scoroutine->std);
        } else {
            /* it is not yield */
            CAT_ASSERT(swow_coroutine_get_previous(scoroutine) == NULL);
            /* current becomes target's origin */
            GC_ADDREF(&current_scoroutine->std);
        }
    } while (0);

    /* switch executor */
    if (scoroutine->coroutine.flags & SWOW_COROUTINE_FLAG_NO_STACK) {
        swow_coroutine_executor_save(current_scoroutine->executor);
        EG(current_execute_data) = NULL; /* make the log stack strace empty */
    } else if (current_scoroutine->coroutine.flags & SWOW_COROUTINE_FLAG_NO_STACK) {
        swow_coroutine_executor_recover(scoroutine->executor);
    } else {
        swow_coroutine_executor_switch(scoroutine);
    }

    /* must be SWOW_COROUTINE_DATA_NULL or else non-void value */
    CAT_ASSERT(zdata != NULL);

    /* we can not use ZVAL_IS_NULL because the zdata maybe a C ptr */
    if (UNEXPECTED(zdata != SWOW_COROUTINE_DATA_NULL)) {
        swow_coroutine_handle_not_null_zdata(scoroutine, swow_coroutine_get_current(), &zdata, cat_false);
    }

    /* resume C coroutine */
    zdata = (zval *) cat_coroutine_jump(&scoroutine->coroutine, zdata);

    /* get from scoroutine */
    scoroutine = swow_coroutine_get_from(current_scoroutine);

    if (UNEXPECTED(scoroutine->coroutine.state == CAT_COROUTINE_STATE_DEAD)) {
        /* release executor resources after coroutine is dead */
        swow_coroutine_close(scoroutine);
        /* delete from global map */
        zend_hash_index_del(SWOW_COROUTINE_G(map), scoroutine->coroutine.id);
    } else {
        swow_coroutine_exector_t *executor = current_scoroutine->executor;
        CAT_ASSERT(executor != NULL);
        /* handle cross exception */
        if (UNEXPECTED(executor->cross_exception != NULL)) {
            swow_coroutine_handle_cross_exception(executor->cross_exception);
            executor->cross_exception = NULL;
        }
    }

    return zdata;
}

SWOW_API cat_data_t *swow_coroutine_resume_standard(cat_coroutine_t *coroutine, cat_data_t *data)
{
    swow_coroutine_t *scoroutine = swow_coroutine_get_from_handle(coroutine);
    zval *zdata = (zval *) data;

    if (EXPECTED(!(scoroutine->coroutine.opcodes & CAT_COROUTINE_OPCODE_CHECKED))) {
        if (UNEXPECTED(!swow_coroutine_jump_precheck(scoroutine, zdata))) {
            return SWOW_COROUTINE_DATA_ERROR;
        }
    }

    return swow_coroutine_jump(scoroutine, zdata);
}

#define SWOW_COROUTINE_JUMP_WITH_ZDATA(operation) do { \
    CAT_COROUTINE_G(current)->opcodes |= SWOW_COROUTINE_OPCODE_ACCEPT_ZDATA; \
    zdata = (operation); \
    if (UNEXPECTED(zdata == SWOW_COROUTINE_DATA_ERROR)) { \
        CAT_COROUTINE_G(current)->opcodes &= ~SWOW_COROUTINE_OPCODE_ACCEPT_ZDATA; \
        if (retval != NULL) { \
            ZVAL_NULL(retval); \
        } \
        return cat_false; \
    } \
    if (UNEXPECTED(retval == NULL)) { \
        zval_ptr_dtor(zdata); \
    } else { \
        ZVAL_COPY_VALUE(retval, zdata); \
    } \
    return cat_true; \
} while (0)

SWOW_API cat_bool_t swow_coroutine_resume(swow_coroutine_t *scoroutine, zval *zdata, zval *retval)
{
    SWOW_COROUTINE_JUMP_WITH_ZDATA(((zval *) cat_coroutine_resume(&scoroutine->coroutine, zdata)));
}

SWOW_API cat_bool_t swow_coroutine_yield(zval *zdata, zval *retval)
{
    SWOW_COROUTINE_JUMP_WITH_ZDATA(((zval *) cat_coroutine_yield(zdata)));
}

#ifdef SWOW_COROUTINE_ENABLE_CUSTOM_ENTRY
static cat_bool_t swow_coroutine_resume_hardlink(swow_coroutine_t *scoroutine, zval *zdata, zval *retval)
{
    if (UNEXPECTED(SWOW_COROUTINE_G(readonly.enable))) {
        swow_coroutine_resume_deny(NULL, CAT_COROUTINE_DATA_NULL);
        CAT_NEVER_HERE(COROUTINE, "Abort in deny");
    }
    SWOW_COROUTINE_JUMP_WITH_ZDATA(swow_coroutine_resume_standard(&scoroutine->coroutine, zdata));
}

static cat_bool_t swow_coroutine_yield_hardlink(zval *zdata, zval *retval)
{
    swow_coroutine_t *scoroutine = swow_coroutine_get_previous(swow_coroutine_get_current());

    if (unlikely(scoroutine == NULL)) {
        cat_update_last_error_ez("Coroutine has nowhere to go");
        return cat_false;
    }
    return swow_coroutine_resume_hardlink(scoroutine, zdata, retval);
}
#endif

#undef SWOW_COROUTINE_JUMP_WITH_ZDATA

SWOW_API cat_bool_t swow_coroutine_resume_ez(swow_coroutine_t *scoroutine)
{
    return cat_coroutine_resume_ez(&scoroutine->coroutine);
}

SWOW_API cat_bool_t swow_coroutine_yield_ez(void)
{
    return cat_coroutine_yield_ez();
}

/* basic info */

SWOW_API cat_bool_t swow_coroutine_is_available(const swow_coroutine_t *scoroutine)
{
    return cat_coroutine_is_available(&scoroutine->coroutine);
}

SWOW_API cat_bool_t swow_coroutine_is_alive(const swow_coroutine_t *scoroutine)
{
     return cat_coroutine_is_alive(&scoroutine->coroutine);
}

SWOW_API swow_coroutine_t *swow_coroutine_get_from(const swow_coroutine_t *scoroutine)
{
    return swow_coroutine_get_from_handle(scoroutine->coroutine.from);
}

SWOW_API swow_coroutine_t *swow_coroutine_get_previous(const swow_coroutine_t *scoroutine)
{
    return swow_coroutine_get_from_handle(scoroutine->coroutine.previous);
}

/* globals (options) */

SWOW_API size_t swow_coroutine_set_default_stack_page_size(size_t size)
{
    size_t original_size = SWOW_COROUTINE_G(default_stack_page_size);
    SWOW_COROUTINE_G(default_stack_page_size) = swow_coroutine_align_stack_page_size(size);
    return original_size;
}

SWOW_API size_t swow_coroutine_set_default_c_stack_size(size_t size)
{
    return cat_coroutine_set_default_stack_size(size);
}

static cat_data_t *swow_coroutine_resume_deny(cat_coroutine_t *coroutine, cat_data_t *data)
{
    cat_core_error(COROUTINE, "Unexpected coroutine switching");
    /* for compiler */
    return CAT_COROUTINE_DATA_NULL;
}

SWOW_API void swow_coroutine_set_readonly(cat_bool_t enable)
{
    swow_coroutine_readonly_t *readonly = &SWOW_COROUTINE_G(readonly);
    if (enable) {
        readonly->original_create_object = swow_coroutine_ce->create_object;
        readonly->original_resume = cat_coroutine_register_resume(swow_coroutine_resume_deny);
        swow_coroutine_ce->create_object = swow_create_object_deny;
    } else {
        if (
            swow_coroutine_ce->create_object == swow_create_object_deny &&
            readonly->original_create_object != NULL
        ) {
            swow_coroutine_ce->create_object = readonly->original_create_object;
        }
        if (
            cat_coroutine_resume == swow_coroutine_resume_deny &&
            readonly->original_resume != NULL
        ) {
            cat_coroutine_register_resume(readonly->original_resume);
        }
    }
}

/* globals (getter) */

SWOW_API swow_coroutine_t *swow_coroutine_get_current(void)
{
    return swow_coroutine_get_from_handle(CAT_COROUTINE_G(current));
}

SWOW_API swow_coroutine_t *swow_coroutine_get_main(void)
{
    return swow_coroutine_get_from_handle(CAT_COROUTINE_G(main));
}

SWOW_API swow_coroutine_t *swow_coroutine_get_scheduler(void)
{
    return swow_coroutine_get_from_handle(CAT_COROUTINE_G(scheduler));
}

/* scheduler */

SWOW_API cat_bool_t swow_coroutine_scheduler_run(swow_coroutine_t *scheduler)
{
    if (!cat_coroutine_scheduler_run(&scheduler->coroutine)) {
        return cat_false;
    }
    /* gain full control */
    zend_hash_index_del(SWOW_COROUTINE_G(map), scheduler->coroutine.id);
    /* solve refcount (being disturbed by exchange) */
    GC_DELREF(&swow_coroutine_get_current()->std);
    GC_DELREF(&scheduler->std);

    return cat_true;
}

SWOW_API swow_coroutine_t *swow_coroutine_scheduler_stop(void)
{
    return swow_coroutine_get_from_handle(cat_coroutine_scheduler_stop());
}

SWOW_API cat_bool_t swow_coroutine_is_scheduler(swow_coroutine_t *scoroutine)
{
    return !!(scoroutine->coroutine.flags & CAT_COROUTINE_FLAG_SCHEDULER);
}

/* executor switcher */

SWOW_API void swow_coroutine_set_executor_switcher(cat_bool_t enable)
{
    swow_coroutine_t *scoroutine = swow_coroutine_get_current();
    if (!enable) {
        swow_coroutine_executor_save(scoroutine->executor);
        scoroutine->coroutine.flags |= SWOW_COROUTINE_FLAG_NO_STACK;
    } else {
        scoroutine->coroutine.flags &= ~SWOW_COROUTINE_FLAG_NO_STACK;
        swow_coroutine_executor_recover(scoroutine->executor);
    }
}

/* trace */

SWOW_API HashTable *swow_coroutine_get_trace(const swow_coroutine_t *scoroutine, zend_long options, zend_long limit)
{
    HashTable *trace;

    if (EXPECTED(swow_coroutine_is_alive(scoroutine))) {
        SWOW_COROUTINE_DO_SOMETHING_START(scoroutine) {
            trace = swow_get_trace(options, limit);
        } SWOW_COROUTINE_DO_SOMETHING_END();
    } else {
        trace = (HashTable *) &zend_empty_array;
    }

    return trace;
}

SWOW_API smart_str *swow_coroutine_get_trace_to_string(swow_coroutine_t *scoroutine, smart_str *str, zend_long options, zend_long limit)
{
    if (EXPECTED(swow_coroutine_is_alive(scoroutine))) {
        SWOW_COROUTINE_DO_SOMETHING_START(scoroutine) {
            str = swow_get_trace_to_string(str, options, limit);
        } SWOW_COROUTINE_DO_SOMETHING_END();
    }

    return str;
}

SWOW_API zend_string *swow_coroutine_get_trace_as_string(const swow_coroutine_t *scoroutine, zend_long options, zend_long limit)
{
    zend_string *trace;

    if (EXPECTED(swow_coroutine_is_alive(scoroutine))) {
        SWOW_COROUTINE_DO_SOMETHING_START(scoroutine) {
            trace = swow_get_trace_as_string(options, limit);
        } SWOW_COROUTINE_DO_SOMETHING_END();
    } else {
        trace = zend_empty_string;
    }

    return trace;
}

SWOW_API HashTable *swow_coroutine_get_trace_as_list(const swow_coroutine_t *scoroutine, zend_long options, zend_long limit)
{
    HashTable *trace;

    if (EXPECTED(swow_coroutine_is_alive(scoroutine))) {
        SWOW_COROUTINE_DO_SOMETHING_START(scoroutine) {
            trace = swow_get_trace_as_list(options, limit);
        } SWOW_COROUTINE_DO_SOMETHING_END();
    } else {
        trace = (HashTable *) &zend_empty_array;
    }

    return trace;
}

SWOW_API void swow_coroutine_dump(swow_coroutine_t *scoroutine)
{
    zval zscoroutine;

    ZVAL_OBJ(&zscoroutine, &scoroutine->std);
    php_var_dump(&zscoroutine, 0);
}


SWOW_API void swow_coroutine_dump_by_id(cat_coroutine_id_t id)
{
    zval *zscoroutine = zend_hash_index_find(SWOW_COROUTINE_G(map), id);

    if (zscoroutine == NULL) {
        zscoroutine = CAT_COROUTINE_DATA_NULL;
    }
    php_var_dump(zscoroutine, 0);
}

SWOW_API void swow_coroutine_dump_all(void)
{
    zval zmap;

    ZVAL_ARR(&zmap, SWOW_COROUTINE_G(map));
    php_var_dump(&zmap, 0);
}

/* exception */

static void swow_coroutine_handle_cross_exception(zend_object *cross_exception)
{
    zend_object *exception;
    zend_class_entry *ce = cross_exception->ce;
    zval zexception, zprevious_exception, ztmp;

    /* for throw method success */
    GC_ADDREF(cross_exception);

    if (UNEXPECTED(EG(exception) != NULL)) {
        if (instanceof_function(ce, swow_coroutine_kill_exception_ce)) {
            OBJ_RELEASE(EG(exception));
            EG(exception) = NULL;
        }
    }
    exception = swow_object_create(ce);
    ZVAL_OBJ(&zexception, exception);
    ZVAL_OBJ(&zprevious_exception, cross_exception);
    zend_update_property_ex(
        ce, &zexception, ZSTR_KNOWN(ZEND_STR_MESSAGE),
        zend_read_property_ex(ce, &zprevious_exception, ZSTR_KNOWN(ZEND_STR_MESSAGE), 1, &ztmp)
    );
    zend_update_property_ex(
        ce, &zexception, ZSTR_KNOWN(ZEND_STR_CODE),
        zend_read_property_ex(ce, &zprevious_exception, ZSTR_KNOWN(ZEND_STR_CODE), 1, &ztmp)
    );
    if (UNEXPECTED(EG(exception) != NULL)) {
        zend_throw_exception_internal(&zprevious_exception);
        zend_throw_exception_internal(&zexception);
    } else {
        zend_exception_set_previous(exception, cross_exception);
        if (EG(exception) == NULL) {
            zend_throw_exception_internal(&zexception);
        }
    }
}

SWOW_API cat_bool_t swow_coroutine_throw(swow_coroutine_t *scoroutine, zend_object *exception, zval *retval)
{
    if (UNEXPECTED(!instanceof_function(exception->ce, zend_ce_throwable))) {
        cat_update_last_error(CAT_EMISUSE, "Instance of %s is not throwable", ZSTR_VAL(exception->ce->name));
        return cat_false;
    }
    if (UNEXPECTED(!swow_coroutine_is_alive(scoroutine))) {
        cat_update_last_error(CAT_ESRCH, "Coroutine is not alive");
        return cat_false;
    }
    if (UNEXPECTED(swow_coroutine_is_scheduler(scoroutine))) {
        cat_update_last_error(CAT_EMISUSE, "Break scheduler coroutine is not allowed");
        return cat_false;
    }
    if (UNEXPECTED(scoroutine == swow_coroutine_get_current())) {
        zval zexception;
        ZVAL_OBJ(&zexception, exception);
        GC_ADDREF(exception);
        zend_throw_exception_internal(&zexception);
    } else {
        scoroutine->executor->cross_exception = exception;
        if (UNEXPECTED(!swow_coroutine_resume(scoroutine, SWOW_COROUTINE_DATA_NULL, retval))) {
            scoroutine->executor->cross_exception = NULL;
            return cat_false;
        }
    }

    return cat_true;
}

SWOW_API cat_bool_t swow_coroutine_term(swow_coroutine_t *scoroutine, const char *message, zend_long code, zval *retval)
{
    zend_object *exception;
    cat_bool_t success;

    exception = swow_object_create(swow_coroutine_term_exception_ce);
    swow_exception_set_properties(exception, message, code);
    success = swow_coroutine_throw(scoroutine, exception, retval);
    OBJ_RELEASE(exception);

    return success;
}

#ifdef SWOW_COROUTINE_USE_RATED
static cat_data_t swow_coroutine_resume_rated(cat_coroutine_t *coroutine, cat_data_t data)
{
    swow_coroutine_t *scoroutine = swow_coroutine_get_from_handle(coroutine);
    swow_coroutine_t *current_scoroutine = swow_coroutine_get_current();
    swow_coroutine_rated_t *rated = &SWOW_COROUTINE_G(rated);

    /* target + current */
    if (UNEXPECTED((
        scoroutine != rated->dead &&
        scoroutine != rated->killer
    ) || (
        current_scoroutine != rated->dead &&
        current_scoroutine != rated->killer
    ))) {
        return swow_coroutine_resume_deny(coroutine, data);
    }

    return swow_coroutine_resume_standard(coroutine, data);
}
#endif

SWOW_API cat_bool_t swow_coroutine_kill(swow_coroutine_t *scoroutine, const char *message, zend_long code)
{
    zend_object *exception;
    cat_bool_t success;
    zval retval;

    exception = swow_object_create(swow_coroutine_kill_exception_ce);
    swow_exception_set_properties(exception, message, code);
#ifndef SWOW_COROUTINE_USE_RATED
    success = swow_coroutine_throw(scoroutine, exception, &retval);
    CAT_ASSERT(!SWOW_COROUTINE_G(kill_main));
    OBJ_RELEASE(exception);
    if (!success) {
        return cat_false;
    }
    zval_ptr_dtor(&retval);

    return cat_true;
#else
    do {
        swow_coroutine_rated_t *rated = &SWOW_COROUTINE_G(rated);
        /* prevent coroutines from escaping */
        cat_coroutine_resume_t original_resume = cat_coroutine_register_resume(swow_coroutine_resume_rated);
        rated->killer = swow_coroutine_get_current();
        rated->dead = scoroutine;
        success = swow_coroutine_throw(scoroutine, exception, &retval);
        CAT_ASSERT(!SWOW_COROUTINE_G(kill_main));
        /* revert */
        cat_coroutine_register_resume(original_resume);
    } while (0);
    OBJ_RELEASE(exception);
    if (UNEXPECTED(!success)) {
        return cat_false;
    }
    if (UNEXPECTED(swow_coroutine_is_running(scoroutine))) {
        cat_core_error(COROUTINE, "Kill coroutine failed by unknown reason");
    }
    if (UNEXPECTED(!ZVAL_IS_NULL(&retval))) {
        cat_core_error(COROUTINE, "Unexpected return value");
    }

    return cat_true;
#endif
}

#define getThisCoroutine() (swow_coroutine_get_from_object(Z_OBJ_P(ZEND_THIS)))

ZEND_BEGIN_ARG_INFO_EX(arginfo_swow_coroutine___construct, 0, ZEND_RETURN_VALUE, 1)
    ZEND_ARG_CALLABLE_INFO(0, callable, 0)
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, stack_page_size, IS_LONG, 0, "0")
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, c_stack_size, IS_LONG, 0, "0")
ZEND_END_ARG_INFO()

static PHP_METHOD(swow_coroutine, __construct)
{
    swow_coroutine_t *scoroutine = getThisCoroutine();
    zval *zcallable;
    zend_long stack_page_size = 0;
    zend_long c_stack_size = 0;

    if (UNEXPECTED(scoroutine->coroutine.state != CAT_COROUTINE_STATE_INIT)) {
        zend_throw_error(NULL, "%s can only construct once", ZEND_THIS_NAME);
        RETURN_THROWS();
    }

    ZEND_PARSE_PARAMETERS_START(1, 3)
        Z_PARAM_ZVAL(zcallable)
        Z_PARAM_OPTIONAL
        /* TODO: options */
        Z_PARAM_LONG(stack_page_size)
        Z_PARAM_LONG(c_stack_size)
    ZEND_PARSE_PARAMETERS_END();

    if (UNEXPECTED(!swow_coroutine_construct(scoroutine, zcallable, stack_page_size, c_stack_size))) {
        swow_throw_exception_with_last(swow_coroutine_exception_ce);
        RETURN_THROWS();
    }
}

#define SWOW_COROUTINE_DECLARE_RESUME_TRANSFER(zdata) \
    zend_fcall_info fci = empty_fcall_info; \
    zval *zdata = SWOW_COROUTINE_DATA_NULL, _##zdata

#define SWOW_COROUTINE_HANDLE_RESUME_TRANSFER(zdata) \
    if (fci.param_count == 1) { \
        zdata = fci.params; \
    } else if (fci.param_count > 1) { \
        if (UNEXPECTED(scoroutine->coroutine.state != CAT_COROUTINE_STATE_READY)) { \
            zend_throw_error(NULL, "Only one argument allowed when resuming an coroutine which is alive"); \
            RETURN_THROWS(); \
        } \
        zdata = &_##zdata; \
        ZVAL_PTR(zdata, &fci); \
    }

ZEND_BEGIN_ARG_WITH_RETURN_OBJ_INFO_EX(arginfo_swow_coroutine_run, ZEND_RETURN_VALUE, 1, Swow\\Coroutine, 0)
    ZEND_ARG_CALLABLE_INFO(0, callable, 0)
    ZEND_ARG_VARIADIC_INFO(0, data)
ZEND_END_ARG_INFO()

static PHP_METHOD(swow_coroutine, run)
{
    zval *zcallable;
    SWOW_COROUTINE_DECLARE_RESUME_TRANSFER(zdata);

    ZEND_PARSE_PARAMETERS_START(1, -1)
        Z_PARAM_ZVAL(zcallable)
        Z_PARAM_VARIADIC('*', fci.params, fci.param_count)
    ZEND_PARSE_PARAMETERS_END();

    swow_coroutine_t *scoroutine = swow_coroutine_create(zcallable);
    if (UNEXPECTED(scoroutine == NULL)) {
        swow_throw_exception_with_last(swow_coroutine_exception_ce);
        RETURN_THROWS();
    }

    SWOW_COROUTINE_HANDLE_RESUME_TRANSFER(zdata);
    if (UNEXPECTED(!swow_coroutine_resume(scoroutine, zdata, NULL))) {
        /* impossible? */
        swow_throw_exception_with_last(swow_coroutine_exception_ce);
        zend_object_release(&scoroutine->std);
        RETURN_THROWS();
    }
    RETURN_OBJ(&scoroutine->std);
}

ZEND_BEGIN_ARG_INFO_EX(arginfo_swow_coroutine_resume, 0, ZEND_RETURN_VALUE, 0)
    ZEND_ARG_VARIADIC_INFO(0, data)
ZEND_END_ARG_INFO()

static PHP_METHOD(swow_coroutine, resume)
{
    swow_coroutine_t *scoroutine = getThisCoroutine();
    SWOW_COROUTINE_DECLARE_RESUME_TRANSFER(zdata);
    cat_bool_t ret;

    ZEND_PARSE_PARAMETERS_START(0, -1)
        Z_PARAM_VARIADIC('*', fci.params, fci.param_count)
    ZEND_PARSE_PARAMETERS_END();

    SWOW_COROUTINE_HANDLE_RESUME_TRANSFER(zdata);

#ifdef SWOW_COROUTINE_ENABLE_CUSTOM_ENTRY
    if (UNEXPECTED(SWOW_COROUTINE_G(custom_entry) != NULL)) {
        ret = swow_coroutine_resume_hardlink(scoroutine, zdata, return_value);
    } else
#endif
     ret = swow_coroutine_resume(scoroutine, zdata, return_value);

    if (UNEXPECTED(!ret)) {
        swow_throw_exception_with_last(swow_coroutine_exception_ce);
        RETURN_THROWS();
    }
}

#undef SWOW_COROUTINE_DECLARE_RESUME_TRANSFER
#undef SWOW_COROUTINE_HANDLE_RESUME_TRANSFER

ZEND_BEGIN_ARG_INFO_EX(arginfo_swow_coroutine_yield, 0, ZEND_RETURN_VALUE, 0)
    ZEND_ARG_INFO_WITH_DEFAULT_VALUE(0, data, "null")
ZEND_END_ARG_INFO()

static PHP_METHOD(swow_coroutine, yield)
{
    zval *zdata = SWOW_COROUTINE_DATA_NULL;
    cat_bool_t ret;

    ZEND_PARSE_PARAMETERS_START(0, 1)
        Z_PARAM_OPTIONAL
        Z_PARAM_ZVAL(zdata)
    ZEND_PARSE_PARAMETERS_END();

#ifdef SWOW_COROUTINE_ENABLE_CUSTOM_ENTRY
    if (UNEXPECTED(SWOW_COROUTINE_G(custom_entry) != NULL)) {
        ret = swow_coroutine_yield_hardlink(zdata, return_value);
    } else
#endif
    ret = swow_coroutine_yield(zdata, return_value);

    if (UNEXPECTED(!ret)) {
        swow_throw_exception_with_last(swow_coroutine_exception_ce);
        RETURN_THROWS();
    }
}

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_swow_coroutine_getId, ZEND_RETURN_VALUE, 0, IS_LONG, 0)
ZEND_END_ARG_INFO()

static PHP_METHOD(swow_coroutine, getId)
{
    ZEND_PARSE_PARAMETERS_NONE();

    RETURN_LONG(getThisCoroutine()->coroutine.id);
}

#define SWOW_COROUTINE_GETTER(getter) do { \
    ZEND_PARSE_PARAMETERS_NONE(); \
    swow_coroutine_t *scoroutine = getter; \
    if (UNEXPECTED(scoroutine == NULL)) { \
        RETURN_NULL(); \
    } \
    GC_ADDREF(&scoroutine->std); \
    RETURN_OBJ(&scoroutine->std); \
} while (0)

ZEND_BEGIN_ARG_WITH_RETURN_OBJ_INFO_EX(arginfo_swow_coroutine_getCoroutine, ZEND_RETURN_VALUE, 0, Swow\\Coroutine, 0)
ZEND_END_ARG_INFO()

static PHP_METHOD_EX(swow_coroutine, getCoroutine, swow_coroutine_t *scoroutine)
{
    ZEND_PARSE_PARAMETERS_NONE();

    if (UNEXPECTED(scoroutine == NULL)) {
        RETURN_NULL();
    }

    GC_ADDREF(&scoroutine->std);
    RETURN_OBJ(&scoroutine->std);
}

#define arginfo_swow_coroutine_getCurrent arginfo_swow_coroutine_getCoroutine

static PHP_METHOD(swow_coroutine, getCurrent)
{
    PHP_METHOD_CALL(swow_coroutine, getCoroutine, swow_coroutine_get_current());
}

#define arginfo_swow_coroutine_getMain arginfo_swow_coroutine_getCoroutine

static PHP_METHOD(swow_coroutine, getMain)
{
    PHP_METHOD_CALL(swow_coroutine, getCoroutine, swow_coroutine_get_main());
}

#define arginfo_swow_coroutine_getPrevious arginfo_swow_coroutine_getCoroutine

static PHP_METHOD(swow_coroutine, getPrevious)
{
    PHP_METHOD_CALL(swow_coroutine, getCoroutine, swow_coroutine_get_previous(getThisCoroutine()));
}

#undef SWOW_COROUTINE_GETTER

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_swow_coroutine_getLong, ZEND_RETURN_VALUE, 0, IS_LONG, 0)
ZEND_END_ARG_INFO()

#define arginfo_swow_coroutine_getState arginfo_swow_coroutine_getLong

static PHP_METHOD(swow_coroutine, getState)
{
    ZEND_PARSE_PARAMETERS_NONE();

    RETURN_LONG(getThisCoroutine()->coroutine.state);
}

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_swow_coroutine_getString, ZEND_RETURN_VALUE, 0, IS_STRING, 0)
ZEND_END_ARG_INFO()

#define arginfo_swow_coroutine_getStateName arginfo_swow_coroutine_getString

static PHP_METHOD(swow_coroutine, getStateName)
{
    ZEND_PARSE_PARAMETERS_NONE();

    RETURN_STRING(cat_coroutine_get_state_name(&getThisCoroutine()->coroutine));
}

#define arginfo_swow_coroutine_getElapsed arginfo_swow_coroutine_getLong

static PHP_METHOD(swow_coroutine, getElapsed)
{
    ZEND_PARSE_PARAMETERS_NONE();

    RETURN_LONG(cat_coroutine_get_elapsed(&getThisCoroutine()->coroutine));
}

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_swow_coroutine_getBool, ZEND_RETURN_VALUE, 0, _IS_BOOL, 0)
ZEND_END_ARG_INFO()

#define arginfo_swow_coroutine_isAvailable arginfo_swow_coroutine_getBool

static PHP_METHOD(swow_coroutine, isAvailable)
{
    ZEND_PARSE_PARAMETERS_NONE();

    RETURN_BOOL(swow_coroutine_is_available(getThisCoroutine()));
}

#define arginfo_swow_coroutine_isAlive arginfo_swow_coroutine_getBool

static PHP_METHOD(swow_coroutine, isAlive)
{
    ZEND_PARSE_PARAMETERS_NONE();

    RETURN_BOOL(swow_coroutine_is_alive(getThisCoroutine()));
}

#define SWOW_COROUTINE_GET_TRACE_PARAMETERS_PARSER() \
    zend_long options = DEBUG_BACKTRACE_PROVIDE_OBJECT; \
    zend_long limit = 0; \
    ZEND_PARSE_PARAMETERS_START(0, 2) \
        Z_PARAM_OPTIONAL \
        Z_PARAM_LONG(options) \
        Z_PARAM_LONG(limit) \
    ZEND_PARSE_PARAMETERS_END_EX(RETURN_FALSE);

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_swow_coroutine_getTrace, ZEND_RETURN_VALUE, 0, IS_ARRAY, 0)
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, options, IS_LONG, 0, CAT_TO_STR(DEBUG_BACKTRACE_PROVIDE_OBJECT))
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, limit, IS_LONG, 0, "0")
ZEND_END_ARG_INFO()

static PHP_METHOD(swow_coroutine, getTrace)
{
    SWOW_COROUTINE_GET_TRACE_PARAMETERS_PARSER();

    RETURN_ARR(swow_coroutine_get_trace(getThisCoroutine(), options, limit));
}

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_swow_coroutine_getTraceAsString, ZEND_RETURN_VALUE, 0, IS_STRING, 0)
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, options, IS_LONG, 0, CAT_TO_STR(DEBUG_BACKTRACE_PROVIDE_OBJECT))
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, limit, IS_LONG, 0, "0")
ZEND_END_ARG_INFO()

static PHP_METHOD(swow_coroutine, getTraceAsString)
{
    SWOW_COROUTINE_GET_TRACE_PARAMETERS_PARSER();

    RETURN_STR(swow_coroutine_get_trace_as_string(getThisCoroutine(), options, limit));
}

#define arginfo_swow_coroutine_getTraceAsList arginfo_swow_coroutine_getTrace

static PHP_METHOD(swow_coroutine, getTraceAsList)
{
    SWOW_COROUTINE_GET_TRACE_PARAMETERS_PARSER();

    RETURN_ARR(swow_coroutine_get_trace_as_list(getThisCoroutine(), options, limit));
}

#undef SWOW_COROUTINE_GET_TRACE_PARAMETERS_PARSER

ZEND_BEGIN_ARG_INFO_EX(arginfo_swow_coroutine_throw, 0, ZEND_RETURN_VALUE, 1)
    ZEND_ARG_OBJ_INFO(0, throwable, Throwable, 0)
ZEND_END_ARG_INFO()

static PHP_METHOD(swow_coroutine, throw)
{
    zval *zexception;

    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_OBJECT_OF_CLASS(zexception, zend_ce_throwable)
    ZEND_PARSE_PARAMETERS_END();

    if (UNEXPECTED(!swow_coroutine_throw(getThisCoroutine(), Z_OBJ_P(zexception), return_value)))  {
        swow_throw_exception_with_last(swow_coroutine_exception_ce);
        RETURN_THROWS();
    }
}

#define SWOW_COROUTINE_MESSAGE_AND_CODE_PARAMETERS_PARSER() \
    char *message = NULL; \
    size_t message_length = 0; \
    zend_long code = ~0; \
    ZEND_PARSE_PARAMETERS_START(0, 2) \
        Z_PARAM_OPTIONAL \
        Z_PARAM_STRING(message, message_length) \
        Z_PARAM_LONG(code) \
    ZEND_PARSE_PARAMETERS_END(); \

ZEND_BEGIN_ARG_INFO_EX(arginfo_swow_coroutine_throwCrossException, 0, ZEND_RETURN_VALUE, 0)
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, message, IS_STRING, 0, "null")
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, code, IS_LONG, 0, "null")
ZEND_END_ARG_INFO()

#define arginfo_swow_coroutine_term arginfo_swow_coroutine_throwCrossException

static PHP_METHOD(swow_coroutine, term)
{
    SWOW_COROUTINE_MESSAGE_AND_CODE_PARAMETERS_PARSER();

    if (UNEXPECTED(!swow_coroutine_term(getThisCoroutine(), message, code, return_value)))  {
        swow_throw_exception_with_last(swow_coroutine_exception_ce);
        RETURN_THROWS();
    }
}

#define arginfo_swow_coroutine_kill arginfo_swow_coroutine_throwCrossException

static PHP_METHOD(swow_coroutine, kill)
{
    SWOW_COROUTINE_MESSAGE_AND_CODE_PARAMETERS_PARSER();

    if (UNEXPECTED(!swow_coroutine_kill(getThisCoroutine(), message, code))) {
        swow_throw_exception_with_last(swow_coroutine_exception_ce);
        RETURN_THROWS();
    }
}

#undef SWOW_COROUTINE_MESSAGE_AND_CODE_PARAMETERS_PARSER

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_swow_coroutine_count, ZEND_RETURN_VALUE, 0, IS_LONG, 0)
ZEND_END_ARG_INFO()

static PHP_METHOD(swow_coroutine, count)
{
    ZEND_PARSE_PARAMETERS_NONE();

    RETURN_LONG(zend_hash_num_elements(SWOW_COROUTINE_G(map)));
}

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_swow_coroutine_getAll, ZEND_RETURN_VALUE, 0, IS_ARRAY, 0)
ZEND_END_ARG_INFO()

static PHP_METHOD(swow_coroutine, getAll)
{
    HashTable *map = SWOW_COROUTINE_G(map);

    ZEND_PARSE_PARAMETERS_NONE();

    RETURN_ARR(zend_array_dup(map));
}

#ifdef SWOW_COROUTINE_ENABLE_CUSTOM_ENTRY
ZEND_BEGIN_ARG_INFO_EX(arginfo_swow_coroutine_extends, 0, ZEND_RETURN_VALUE, 1)
    ZEND_ARG_TYPE_INFO(0, class, IS_STRING, 0)
ZEND_END_ARG_INFO()

static cat_data_t swow_coroutine_custom_resume(cat_coroutine_t *coroutine, cat_data_t data)
{
    swow_coroutine_t *scoroutine = swow_coroutine_get_from_handle(coroutine);
    zval *retval = scoroutine->executor ? &scoroutine->executor->zdata : NULL;
    zval zscoroutine;

    // TOOD: if PHP and zdata == IS_PTR

    ZVAL_OBJ(&zscoroutine, &scoroutine->std);
    swow_call_method_with_1_params(&zscoroutine, scoroutine->std.ce, NULL, "resume", retval, (zval *) data);

    return retval;
}

static PHP_METHOD(swow_coroutine, extends)
{
    zend_string *name;
    zend_class_entry *ce;

    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_STR(name)
    ZEND_PARSE_PARAMETERS_END();

    ce = zend_lookup_class(name);
    if (UNEXPECTED(ce == NULL)) {
        swow_throw_error(swow_coroutine_error_ce, "Class %s dose not exist", ZSTR_VAL(name));
        RETURN_THROWS();
    }
    if (ce == swow_coroutine_ce) {
        SWOW_COROUTINE_G(custom_entry) = NULL;
        cat_coroutine_register_resume(swow_coroutine_resume_standard);
        return;
    }
    if (UNEXPECTED(!instanceof_function(ce, swow_coroutine_ce))) {
        swow_throw_error(swow_coroutine_error_ce, "Class %s must extends %s", ZSTR_VAL(name), ZSTR_VAL(swow_coroutine_error_ce->name));
        RETURN_THROWS();
    }
    SWOW_COROUTINE_G(custom_entry) = ce;
    cat_coroutine_register_resume(swow_coroutine_custom_resume);
}
#endif

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_swow_coroutine___debugInfo, ZEND_RETURN_VALUE, 0, IS_ARRAY, 0)
ZEND_END_ARG_INFO()

static PHP_METHOD(swow_coroutine, __debugInfo)
{
    swow_coroutine_t *scoroutine = getThisCoroutine();
    cat_coroutine_t *coroutine = &scoroutine->coroutine;
    zval zdebug_info;
    char *tmp;

    ZEND_PARSE_PARAMETERS_NONE();

    array_init(&zdebug_info);
    add_assoc_long(&zdebug_info, "id", coroutine->id);
    add_assoc_string(&zdebug_info, "state", cat_coroutine_get_state_name(coroutine));
    tmp = cat_time_format_msec(cat_coroutine_get_elapsed(coroutine));
    add_assoc_string(&zdebug_info, "elapsed", tmp);
    cat_free(tmp);
    if (swow_coroutine_is_alive(scoroutine)) {
        const zend_long options = DEBUG_BACKTRACE_PROVIDE_OBJECT;
        const zend_long limit = 0;
        smart_str str = { 0 };
        smart_str_appendc(&str, '\n');
        swow_coroutine_get_trace_to_string(scoroutine, &str, options, limit);
        smart_str_appendc(&str, '\n');
        smart_str_0(&str);
        add_assoc_str(&zdebug_info, "trace", str.s);
    }

    RETURN_DEBUG_INFO_WITH_PROPERTIES(&zdebug_info);
}

static const zend_function_entry swow_coroutine_methods[] = {
    PHP_ME(swow_coroutine, __construct,         arginfo_swow_coroutine___construct,         ZEND_ACC_PUBLIC)
    PHP_ME(swow_coroutine, run,                 arginfo_swow_coroutine_run,                 ZEND_ACC_PUBLIC | ZEND_ACC_STATIC)
    PHP_ME(swow_coroutine, resume,              arginfo_swow_coroutine_resume,              ZEND_ACC_PUBLIC)
    PHP_ME(swow_coroutine, yield,               arginfo_swow_coroutine_yield,               ZEND_ACC_PUBLIC | ZEND_ACC_STATIC)
    PHP_ME(swow_coroutine, getId,               arginfo_swow_coroutine_getId,               ZEND_ACC_PUBLIC)
    PHP_ME(swow_coroutine, getCurrent,          arginfo_swow_coroutine_getCurrent,          ZEND_ACC_PUBLIC | ZEND_ACC_STATIC)
    PHP_ME(swow_coroutine, getMain,             arginfo_swow_coroutine_getMain,             ZEND_ACC_PUBLIC | ZEND_ACC_STATIC)
    PHP_ME(swow_coroutine, getPrevious,         arginfo_swow_coroutine_getPrevious,         ZEND_ACC_PUBLIC)
    PHP_ME(swow_coroutine, getState,            arginfo_swow_coroutine_getState,            ZEND_ACC_PUBLIC)
    PHP_ME(swow_coroutine, getStateName,        arginfo_swow_coroutine_getStateName,        ZEND_ACC_PUBLIC)
    PHP_ME(swow_coroutine, getElapsed,          arginfo_swow_coroutine_getElapsed,          ZEND_ACC_PUBLIC)
    PHP_ME(swow_coroutine, isAvailable,         arginfo_swow_coroutine_isAvailable,         ZEND_ACC_PUBLIC)
    PHP_ME(swow_coroutine, isAlive,             arginfo_swow_coroutine_isAlive,             ZEND_ACC_PUBLIC)
    PHP_ME(swow_coroutine, getTrace,            arginfo_swow_coroutine_getTrace,            ZEND_ACC_PUBLIC)
    PHP_ME(swow_coroutine, getTraceAsString,    arginfo_swow_coroutine_getTraceAsString,    ZEND_ACC_PUBLIC)
    PHP_ME(swow_coroutine, getTraceAsList,      arginfo_swow_coroutine_getTraceAsList,      ZEND_ACC_PUBLIC)
    PHP_ME(swow_coroutine, throw,               arginfo_swow_coroutine_throw,               ZEND_ACC_PUBLIC)
    PHP_ME(swow_coroutine, term,                arginfo_swow_coroutine_term,                ZEND_ACC_PUBLIC)
    PHP_ME(swow_coroutine, kill,                arginfo_swow_coroutine_kill,                ZEND_ACC_PUBLIC)
    PHP_ME(swow_coroutine, count,               arginfo_swow_coroutine_count,               ZEND_ACC_PUBLIC | ZEND_ACC_STATIC)
    PHP_ME(swow_coroutine, getAll,              arginfo_swow_coroutine_getAll,              ZEND_ACC_PUBLIC | ZEND_ACC_STATIC)
#ifdef SWOW_COROUTINE_ENABLE_CUSTOM_ENTRY
    PHP_ME(swow_coroutine, extends,             arginfo_swow_coroutine_extends,             ZEND_ACC_PUBLIC | ZEND_ACC_STATIC)
#endif
    /* magic */
    PHP_ME(swow_coroutine, __debugInfo,         arginfo_swow_coroutine___debugInfo,         ZEND_ACC_PUBLIC)
    PHP_FE_END
};

/* handlers */

static HashTable *swow_coroutine_get_gc(zend7_object *object, zval **gc_data, int *gc_count)
{
    swow_coroutine_t *scoroutine = swow_coroutine_get_from_object(Z7_OBJ(object));
    zval *zcallable = scoroutine->executor ? &scoroutine->executor->zcallable : NULL;

    if (zcallable && !ZVAL_IS_NULL(zcallable)) {
        *gc_data = zcallable;
        *gc_count = 1;
    } else {
        *gc_data = NULL;
        *gc_count = 0;
    }

    return zend_std_get_properties(object);
}

/* exception/error */

#define arginfo_swow_coroutine_exception_getCoroutine arginfo_swow_coroutine_getCoroutine

static PHP_METHOD(swow_coroutine_exception, getCoroutine)
{
    ZEND_PARSE_PARAMETERS_NONE();

    RETURN_THIS_PROPERTY("coroutine");
}

static const zend_function_entry swow_coroutine_exception_methods[] = {
    PHP_ME(swow_coroutine_exception, getCoroutine, arginfo_swow_coroutine_exception_getCoroutine, ZEND_ACC_FINAL | ZEND_ACC_PUBLIC)
    PHP_FE_END
};

static zend_object *swow_coroutine_cross_exception_create_object(zend_class_entry *ce)
{
    zend_object *object;
    zval zobject, zcoroutine;

    object = swow_exception_create_object(ce);
    ZVAL_OBJ(&zobject, object);
    ZVAL_OBJ(&zcoroutine, &swow_coroutine_get_current()->std);
    zend_update_property(ce, &zobject, ZEND_STRL("coroutine"), &zcoroutine);

    return object;
}

/* hack error hook */

#if PHP_VERSION_ID < 80000
#define ZEND_ERROR_CB_LAST_ARG_D const char *format, va_list args
#define ZEND_ERROR_CB_LAST_ARG_RELAY format, args
#else
#define ZEND_ERROR_CB_LAST_ARG_D     zend_string *message
#define ZEND_ERROR_CB_LAST_ARG_RELAY message
#endif

static void (*original_zend_error_cb)(int type, const char *error_filename, const uint32_t error_lineno, ZEND_ERROR_CB_LAST_ARG_D);

static void swow_call_original_zend_error_cb(int type, const char *error_filename, const uint32_t error_lineno, ZEND_ERROR_CB_LAST_ARG_D)
{
    if (EXPECTED(original_zend_error_cb != NULL)) {
        zend_try {
            original_zend_error_cb(type, error_filename, error_lineno, ZEND_ERROR_CB_LAST_ARG_RELAY);
        } zend_catch {
            exit(255);
        } zend_end_try();
    }
}

static void swow_coroutine_error_cb(int type, const char *error_filename, const uint32_t error_lineno, ZEND_ERROR_CB_LAST_ARG_D)
{
#if PHP_VERSION_ID >= 80000
    const char *format = ZSTR_VAL(message);
#endif
    zend_string *new_message = NULL;

    if (!SWOW_COROUTINE_G(classic_error_handler)) {
        const char *orginal_type_string = swow_strerrortype(type);
        zend_string *trace = NULL;
        if (strncmp(format, ZEND_STRL("Uncaught ")) == 0) {
            /* hack hook for error in main */
            if (swow_coroutine_get_current() == swow_coroutine_get_main()) {
                /* keep slient for killer */
                if (SWOW_COROUTINE_G(kill_main)) {
                    SWOW_COROUTINE_G(kill_main) = cat_false;
                    return;
                } else {
                    zend_long severity = SWOW_COROUTINE_G(exception_error_severity);
                    if (severity == E_NONE) {
                        return;
                    }
                    type = severity;
                    orginal_type_string = swow_strerrortype(type);
                }
            }
            /* the exception of the coroutines will never cause the process to exit */
            if (type & E_FATAL_ERRORS) {
                type = E_WARNING;
            }
        } else {
            if (EG(current_execute_data) != NULL) {
                trace = swow_get_trace_as_string(DEBUG_BACKTRACE_PROVIDE_OBJECT, 0);
            }
        }
        do {
            /* Notice: current coroutine is NULL before RINIT */
            swow_coroutine_t *scoroutine = swow_coroutine_get_current();
            cat_coroutine_id_t id = scoroutine != NULL ? scoroutine->coroutine.id : CAT_COROUTINE_MAIN_ID;

            new_message = zend_strpprintf(0,
                "[%s in R" CAT_COROUTINE_ID_FMT "] %s%s%s%s",
                orginal_type_string,
                id,
                format,
                trace != NULL ? "\nStack trace:\n" : "",
                trace != NULL ? ZSTR_VAL(trace) : "",
                trace != NULL ? "\n  triggered" : ""
            );
#if PHP_VERSION_ID < 80000
            format = ZSTR_VAL(new_message);
#else
            message = new_message;
#endif
        } while (0);
        if (trace != NULL) {
            zend_string_release(trace);
        }
    }
    if (UNEXPECTED(type & E_FATAL_ERRORS)) {
        /* update executor for backtrace */
        if (EG(current_execute_data) != NULL) {
            swow_coroutine_executor_save(swow_coroutine_get_current()->executor);
        }
    }
    swow_call_original_zend_error_cb(type, error_filename, error_lineno, ZEND_ERROR_CB_LAST_ARG_RELAY);
    if (new_message != NULL) {
        zend_string_release(new_message);
    }
}

/* hook exception */

static void (*original_zend_throw_exception_hook)(zval *zexception);

static void swow_zend_throw_exception_hook(zval *zexception)
{
    if (swow_coroutine_get_current() == swow_coroutine_get_main()) {
        if (instanceof_function(Z_OBJCE_P(zexception), swow_coroutine_kill_exception_ce)) {
            SWOW_COROUTINE_G(kill_main) = cat_true;
        }
    }
    if (original_zend_throw_exception_hook != NULL) {
        original_zend_throw_exception_hook(zexception);
    }
}

/* hook exit */

static user_opcode_handler_t original_zend_exit_handler;

static int swow_coroutine_exit_handler(zend_execute_data *execute_data)
{
    const zend_op *opline = EX(opline);
    zval *zstatus = NULL;

    if (opline->op1_type != IS_UNUSED) {
        if (opline->op1_type == IS_CONST) {
            // see: https://github.com/php/php-src/commit/e70618aff6f447a298605d07648f2ce9e5a284f5
#ifdef EX_CONSTANT
            zstatus = EX_CONSTANT(opline->op1);
#else
            zstatus = RT_CONSTANT(opline, opline->op1);
#endif
        } else {
            zstatus = EX_VAR(opline->op1.var);
        }
        if (Z_ISREF_P(zstatus)) {
            zstatus = Z_REFVAL_P(zstatus);
        }
    }
    if (zstatus != NULL && Z_TYPE_P(zstatus) == IS_LONG && Z_LVAL_P(zstatus) != 0) {
        /* exit abnormally */
        zend_long status = Z_LVAL_P(zstatus);
        zend_throw_exception_ex(swow_coroutine_term_exception_ce, status, "Exited with code " ZEND_LONG_FMT, status);
        if (swow_coroutine_get_current() == swow_coroutine_get_main()) {
            EG(exit_status) = status;
        }
    } else {
        /* exit normally */
        if (zstatus != NULL && Z_TYPE_P(zstatus) != IS_LONG) {
            zend_print_zval(zstatus, 0);
        }
        zend_throw_exception(swow_coroutine_kill_exception_ce, NULL, 0);
    }
    /* dtor */
    if ((opline->op1_type) & (IS_TMP_VAR | IS_VAR)) {
        zval_ptr_dtor(zstatus);
    }

    return ZEND_USER_OPCODE_DISPATCH;
}

/* hook silence */

#if SWOW_COROUTINE_SWAP_SILENCE_CONTEXT
static user_opcode_handler_t original_zend_begin_silence_handler;
static user_opcode_handler_t original_zend_end_silence_handler;

static int swow_coroutine_begin_silence_handler(zend_execute_data *execute_data)
{
    swow_coroutine_t *scoroutine = swow_coroutine_get_current();
    scoroutine->executor->error_reporting_for_silence = EG(error_reporting);
    return ZEND_USER_OPCODE_DISPATCH;
}

static int swow_coroutine_end_silence_handler(zend_execute_data *execute_data)
{
    swow_coroutine_t *scoroutine = swow_coroutine_get_current();
    scoroutine->executor->error_reporting_for_silence = E_SILENCE_MAGIC;
    return ZEND_USER_OPCODE_DISPATCH;
}
#endif

/* readonly */
static zval swow_coroutine_data_null;
static zval swow_coroutine_data_error;

int swow_coroutine_module_init(INIT_FUNC_ARGS)
{
    if (!cat_coroutine_module_init()) {
        return FAILURE;
    }

    CAT_GLOBALS_REGISTER(swow_coroutine, CAT_GLOBALS_CTOR(swow_coroutine), NULL);

    ZVAL_NULL(&swow_coroutine_data_null);
    ZVAL_ERROR(&swow_coroutine_data_error);

    SWOW_COROUTINE_G(runtime_state) = SWOW_COROUTINE_RUNTIME_STATE_NONE;

    swow_coroutine_ce = swow_register_internal_class(
        "Swow\\Coroutine", NULL, swow_coroutine_methods,
        &swow_coroutine_handlers, NULL,
        cat_false, cat_false, cat_false,
        swow_coroutine_create_object,
        swow_coroutine_free_object,
        XtOffsetOf(swow_coroutine_t, std)
    );
    swow_coroutine_handlers.get_gc = swow_coroutine_get_gc;
    swow_coroutine_handlers.dtor_obj = swow_coroutine_dtor_object;
    /* constants */
#define SWOW_COROUTINE_STATE_GEN(name, value) \
    zend_declare_class_constant_long(swow_coroutine_ce, ZEND_STRL("STATE_" #name), (value));
    CAT_COROUTINE_STATE_MAP(SWOW_COROUTINE_STATE_GEN)
#undef SWOW_COROUTINE_STATE_GEN

    /* Exception for common errors */
    swow_coroutine_exception_ce = swow_register_internal_class(
        "Swow\\Coroutine\\Exception", swow_exception_ce, NULL, NULL, NULL, cat_true, cat_true, cat_true, NULL, NULL, 0
    );

    /* Exceptions for cross throw */
    swow_coroutine_cross_exception_ce = swow_register_internal_class(
        "Swow\\Coroutine\\CrossException", swow_coroutine_exception_ce, swow_coroutine_exception_methods, NULL, NULL, cat_true, cat_true, cat_true,
        swow_coroutine_cross_exception_create_object, NULL, 0
    );
    zend_declare_property_null(swow_coroutine_cross_exception_ce, ZEND_STRL("coroutine"), ZEND_ACC_PROTECTED);
    /* TODO: zend_declare_property_long(swow_coroutine_error_ce, ZEND_STRL("severity"), 0, ZEND_ACC_PROTECTED); */

    swow_coroutine_term_exception_ce = swow_register_internal_class(
        "Swow\\Coroutine\\TermException", swow_coroutine_cross_exception_ce, NULL, NULL, NULL, cat_true, cat_true, cat_true, NULL, NULL, 0
    );
    swow_coroutine_kill_exception_ce = swow_register_internal_class(
        "Swow\\Coroutine\\KillException", swow_coroutine_cross_exception_ce, NULL, NULL, NULL, cat_true, cat_true, cat_true, NULL, NULL, 0
    );
    zend_class_implements(swow_coroutine_kill_exception_ce, 1, swow_uncatchable_ce);

    /* hook zend_error_cb */
    do {
        original_zend_error_cb = zend_error_cb;
        zend_error_cb = swow_coroutine_error_cb;
    } while (0);

    /* hook zend_throw_exception_hook */
    do {
        original_zend_throw_exception_hook = zend_throw_exception_hook;
        zend_throw_exception_hook = swow_zend_throw_exception_hook;
    } while (0);

    /* hook exit */
    do {
        original_zend_exit_handler = zend_get_user_opcode_handler(ZEND_EXIT);
        zend_set_user_opcode_handler(ZEND_EXIT, swow_coroutine_exit_handler);
    } while (0);

#if SWOW_COROUTINE_SWAP_SILENCE_CONTEXT
    /* hook silence */
    do {
        original_zend_begin_silence_handler = zend_get_user_opcode_handler(ZEND_BEGIN_SILENCE);
        zend_set_user_opcode_handler(ZEND_BEGIN_SILENCE, swow_coroutine_begin_silence_handler);
        original_zend_end_silence_handler = zend_get_user_opcode_handler(ZEND_END_SILENCE);
        zend_set_user_opcode_handler(ZEND_END_SILENCE, swow_coroutine_end_silence_handler);
    } while (0);
#endif

    return SUCCESS;
}

#ifdef SWOW_COROUTINE_HOOK_ZEND_EXUECTE_EX
static void swow_execute_ex(zend_execute_data *execute_data)
{
    if (PG(modules_activated) && EG(current_execute_data) && EG(current_execute_data)->prev_execute_data == NULL) {
        zval retval;
        /* revert to original (just hook the main) */
        zend_execute_ex = SWOW_COROUTINE_G(original_zend_execute_ex);
        /* set return_value */
        execute_data->return_value = &retval;
        /* execute code of main */
        zend_execute_ex(execute_data);
        /* as same as coroutine finished */
        if (UNEXPECTED(EG(exception) != NULL)) {
            swow_coroutine_function_handle_exception();
        }
    #if SWOW_COROUTINE_SWAP_OUTPUT_GLOBALS
        if (UNEXPECTED(OG(handlers).elements != NULL)) {
            swow_coroutine_output_globals_end();
        }
    #endif
        zval_ptr_dtor(&retval);
        cat_coroutine_lock();
    } else {
        SWOW_COROUTINE_G(original_zend_execute_ex)(execute_data);
        return;
    }
}
#endif

int swow_coroutine_runtime_init(INIT_FUNC_ARGS)
{
    if (!cat_coroutine_runtime_init()) {
        return FAILURE;
    }

    cat_coroutine_register_common_wrappers(
        swow_coroutine_resume_standard,
        &swow_coroutine_data_null,
        &swow_coroutine_data_error
    );

    SWOW_COROUTINE_G(default_stack_page_size) = SWOW_COROUTINE_DEFAULT_STACK_PAGE_SIZE; /* TODO: get php.ini */
    SWOW_COROUTINE_G(classic_error_handler) = cat_false; /* TODO: get php.ini */
    SWOW_COROUTINE_G(exception_error_severity) = E_ERROR; /* TODO: get php.ini */

    SWOW_COROUTINE_G(runtime_state) = SWOW_COROUTINE_RUNTIME_STATE_RUNNING;

    SWOW_COROUTINE_G(readonly).original_create_object = NULL;
    SWOW_COROUTINE_G(readonly).original_resume = NULL;

    /* create scoroutine map */
    do {
        zval ztmp;
        array_init(&ztmp);
        SWOW_COROUTINE_G(map) = Z_ARRVAL(ztmp);
    } while (0);

    /* create main scoroutine */
    do {
        swow_coroutine_t *scoroutine = swow_coroutine_get_from_object(swow_object_create(swow_coroutine_ce));
        /* construct (make sure the follow-up logic works) */
        scoroutine->executor = (swow_coroutine_exector_t *) ecalloc(1, sizeof(*scoroutine->executor));
        ZVAL_NULL(&scoroutine->executor->zcallable);
        /* register first (sync coroutine info) */
        SWOW_COROUTINE_G(original_main) = cat_coroutine_register_main(&scoroutine->coroutine);
        /* add main scoroutine to the map */
        do {
            zval zscoroutine;
            ZVAL_OBJ(&zscoroutine, &scoroutine->std);
            zend_hash_index_update(SWOW_COROUTINE_G(map), scoroutine->coroutine.id, &zscoroutine);
            /* GC_ADDREF(&scoroutine->std); // we have 1 ref by create*/
        } while (0);
    } while (0);

#ifdef SWOW_COROUTINE_HOOK_ZEND_EXUECTE_EX
#if ZTS
#error "unsupported"
#endif
    /* hook zend_execute_ex */
    do {
        SWOW_COROUTINE_G(original_zend_execute_ex) = zend_execute_ex;
        zend_execute_ex = swow_execute_ex;
    } while (0);
#endif

    return SUCCESS;
}

// TODO: killall API
#ifdef CAT_DO_NOT_OPTIMIZE
static void swow_coroutines_kill_destructor(zval *zscoroutine)
{
    swow_coroutine_t *scoroutine = swow_coroutine_get_from_object(Z_OBJ_P(zscoroutine));
    CAT_ASSERT(swow_coroutine_is_alive(scoroutine));
    if (UNEXPECTED(!swow_coroutine_kill(scoroutine, "Coroutine is forced to kill when the runtime shutdown", ~0))) {
        cat_core_error(COROUTINE, "Execute kill destructor failed, reason: %s", cat_get_last_error_message());
    }
    zend_object_release(&scoroutine->std);
}
#endif

int swow_coroutine_runtime_shutdown(SHUTDOWN_FUNC_ARGS)
{
    SWOW_COROUTINE_G(runtime_state) = SWOW_COROUTINE_RUNTIME_STATE_IN_SHUTDOWN;

    do {
        swow_coroutine_t *main_scoroutine = swow_coroutine_get_main();

#ifdef CAT_DO_NOT_OPTIMIZE /* the optimization deps on event scheduler */
        /* destruct scoroutines and map (except main) */
        do {
            HashTable *internal_map = SWOW_COROUTINE_G(map);
            do {
                /* kill first (for memory safety) */
                HashTable *map = zend_array_dup(internal_map);
                /* kill all coroutines */
                zend_hash_index_del(map, main_scoroutine->coroutine.id);
                map->pDestructor = swow_coroutines_kill_destructor;
                zend_array_destroy(map);
            } while (zend_hash_num_elements(internal_map) != 1);
        } while (0);
#endif

        /* check scheduler */
        if (swow_coroutine_get_scheduler() != NULL) {
            cat_core_error_with_last(COROUTINE, "Scheduler is still running");
        }

        if (CAT_COROUTINE_G(active_count) != 1) {
            cat_core_error(COROUTINE, "Unexpected number of coroutines (%u)", CAT_COROUTINE_G(active_count));
        }

        /* coroutine switching should no longer occur */
        swow_coroutine_set_readonly(cat_true);

        /* revert globals main */
        cat_coroutine_register_main(SWOW_COROUTINE_G(original_main));
        /* hack way to close the main */
#define SWOW_COROUTINE_PRE_CLOSE_MAIN(main_scoroutine) do { \
        (main_scoroutine)->coroutine.state = CAT_COROUTINE_STATE_READY; \
        (main_scoroutine)->executor->vm_stack = NULL; \
} while (0)
        SWOW_COROUTINE_PRE_CLOSE_MAIN(main_scoroutine);
#undef  SWOW_COROUTINE_PRE_CLOSE_MAIN
        /* destroy all (include main) */
        zend_array_destroy(SWOW_COROUTINE_G(map));
        SWOW_COROUTINE_G(map) = NULL;
    } while (0);

    SWOW_COROUTINE_G(runtime_state) = SWOW_COROUTINE_RUNTIME_STATE_NONE;

    return SUCCESS;
}