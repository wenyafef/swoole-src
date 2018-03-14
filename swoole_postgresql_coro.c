#include "php_swoole.h"
#include "swoole_postgresql_core.h"
#include "swoole_coroutine.h"

static PHP_METHOD(swoole_postgresql_coro, __construct);
static PHP_METHOD(swoole_postgresql_coro, __destruct);
static PHP_METHOD(swoole_postgresql_coro, connect);
static PHP_METHOD(swoole_postgresql_coro, pgQuery);
static PHP_METHOD(swoole_postgresql_coro, pg_fetch_all);

static void _close_pgsql_link(zend_resource *rsrc);
static void _free_result(zend_resource *rsrc);
static int swoole_pgsql_coro_onRead(swReactor *reactor, swEvent *event);
static int swoole_pgsql_coro_onWrite(swReactor *reactor, swEvent *event);
//static int swoole_pgsql_coro_onError(swReactor *reactor, swEvent *event);
int php_pgsql_result2array(PGresult *pg_result, zval *ret_array, long result_type);
static int swoole_pgsql_coro_close(zval *this, int fd);

ZEND_BEGIN_ARG_INFO_EX(arginfo_swoole_void, 0, 0, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_pg_connect, 0, 0, -1)
    ZEND_ARG_INFO(0, conninfo)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_pg_query, 0, 0, 0)
    ZEND_ARG_INFO(0, connection)
    ZEND_ARG_INFO(0, query)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_pg_fetch_all, 0, 0, 0)
    ZEND_ARG_INFO(0, result)
    ZEND_ARG_INFO(0, result_type)
ZEND_END_ARG_INFO()

static const zend_function_entry swoole_postgresql_coro_methods[] =
{
    PHP_ME(swoole_postgresql_coro, __construct, arginfo_swoole_void, ZEND_ACC_PUBLIC | ZEND_ACC_CTOR)
    PHP_ME(swoole_postgresql_coro, connect, arginfo_pg_connect, ZEND_ACC_PUBLIC | ZEND_ACC_CTOR)
    PHP_ME(swoole_postgresql_coro, pgQuery, arginfo_pg_query, ZEND_ACC_PUBLIC | ZEND_ACC_CTOR)
    PHP_ME(swoole_postgresql_coro, pg_fetch_all, arginfo_pg_fetch_all, ZEND_ACC_PUBLIC | ZEND_ACC_CTOR)
    PHP_ME(swoole_postgresql_coro, __destruct, arginfo_swoole_void, ZEND_ACC_PUBLIC | ZEND_ACC_CTOR)
    PHP_FE_END
};

static zend_class_entry swoole_postgresql_coro_ce;
static zend_class_entry *swoole_postgresql_coro_class_entry_ptr;
static int le_link , le_result;

void swoole_postgresql_coro_init(int module_number TSRMLS_DC)
{

    INIT_CLASS_ENTRY(swoole_postgresql_coro_ce, "Swoole\\Coroutine\\PostgreSql", swoole_postgresql_coro_methods);
    le_link = zend_register_list_destructors_ex(_close_pgsql_link, NULL, "pgsql link", module_number);
    le_result = zend_register_list_destructors_ex(_free_result, NULL, "pgsql result", module_number);
    swoole_postgresql_coro_class_entry_ptr = zend_register_internal_class(&swoole_postgresql_coro_ce TSRMLS_CC);
    if (SWOOLE_G(use_shortname))
    {
        sw_zend_register_class_alias("Co\\PostgreSql", swoole_postgresql_coro_class_entry_ptr);
    }
}
static PHP_METHOD(swoole_postgresql_coro, __construct)
{
    PGobject *PGobject;
    PGobject = emalloc(sizeof(PGobject));
    bzero(PGobject, sizeof(PGobject));

    swoole_set_object(getThis(), PGobject);

}
static PHP_METHOD(swoole_postgresql_coro, connect)
{
    zval *conninfo;
    PGconn * pgsql;

    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_ZVAL(conninfo)
    ZEND_PARSE_PARAMETERS_END();

    pgsql = PQconnectStart(Z_STRVAL_P(conninfo));
    //pgsql = PQconnectdb(Z_STRVAL_P(conninfo));
    int fd =  PQsocket(pgsql);

    php_printf("sock :%d \n",fd);
    //PQconnectPoll(pgsql);
    php_swoole_check_reactor();
    if (!swReactor_handle_isset(SwooleG.main_reactor, PHP_SWOOLE_FD_POSTGRESQL))
    {
        php_printf("来reactor了");
        SwooleG.main_reactor->setHandle(SwooleG.main_reactor, PHP_SWOOLE_FD_POSTGRESQL | SW_EVENT_READ, swoole_pgsql_coro_onRead);
        SwooleG.main_reactor->setHandle(SwooleG.main_reactor, PHP_SWOOLE_FD_POSTGRESQL | SW_EVENT_WRITE, swoole_pgsql_coro_onWrite);
        //SwooleG.main_reactor->setHandle(SwooleG.main_reactor, PHP_SWOOLE_FD_POSTGRESQL | SW_EVENT_ERROR, swoole_pgsql_coro_onError);
    }

    if (SwooleG.main_reactor->add(SwooleG.main_reactor, fd, PHP_SWOOLE_FD_POSTGRESQL | SW_EVENT_WRITE) < 0)
    {
        //swoole_php_fatal_error(E_WARNING, "swoole_event_add failed. Erorr: %s[%d].", redis->context->errstr, redis->context->err);
        RETURN_FALSE;
    }

    PGobject *PGobject = swoole_get_object(getThis());
    PGobject->fd = fd;
    PGobject->conn = pgsql;
    PGobject->status = CONNECTION_STARTED;
    PGobject->object = getThis();


    //int no_block = PQsetnonblocking(pgsql , 1);
    //php_printf("isblock:%d",no_block);
    //PQconnectPoll(pgsql);
    //get fd

    if (pgsql==NULL || PQstatus(pgsql)==CONNECTION_BAD) {
        swWarn("Unable to connect to PostgreSQL server: [%s]",pgsql);
        if (pgsql) {
            PQfinish(pgsql);
        }
        //goto err;
        RETURN_FALSE;
    }

    php_printf("来了2");








    swConnection *_socket = swReactor_get(SwooleG.main_reactor, fd);
    _socket->object = PGobject;
    _socket->active = 0;

    php_context *sw_current_context = swoole_get_property(getThis(), 0);
    if (!sw_current_context)
    {
        sw_current_context = emalloc(sizeof(php_context));
        swoole_set_property(getThis(), 0, sw_current_context);
    }
    sw_current_context->state = SW_CORO_CONTEXT_RUNNING;
    sw_current_context->onTimeout = NULL;
    #if PHP_MAJOR_VERSION < 7
    sw_current_context->coro_params = getThis();
    #else
    sw_current_context->coro_params = *getThis();
    #endif
    /*
    if (redis->timeout > 0)
    {
        php_swoole_check_timer((int) (redis->timeout * 1000));
        redis->timer = SwooleG.timer.add(&SwooleG.timer, (int) (redis->timeout * 1000), 0, sw_current_context, swoole_redis_coro_onTimeout);
    }
     */
    coro_save(sw_current_context);
    coro_yield();
    //RETVAL_RES(zend_register_resource(pgsql, le_link));
    //return;

/*
err:
    zval_dtor(conninfo);
    RETURN_FALSE;
    */
}

static int swoole_pgsql_coro_onWrite(swReactor *reactor, swEvent *event)
{
    php_printf("来了3");
#if PHP_MAJOR_VERSION < 7
    TSRMLS_FETCH_FROM_CTX(sw_thread_ctx ? sw_thread_ctx : NULL);
#endif

    if (event->socket->active)
    {
        return swReactor_onWrite(SwooleG.main_reactor, event);
    }

    socklen_t len = sizeof(SwooleG.error);
    if (getsockopt(event->fd, SOL_SOCKET, SO_ERROR, &SwooleG.error, &len) < 0)
    {
        swWarn("getsockopt(%d) failed. Error: %s[%d]", event->fd, strerror(errno), errno);
        return SW_ERR;
    }

    PGobject *PGobject = event->socket->object;


    // wait the connection ok
    PostgresPollingStatusType flag = PGRES_POLLING_WRITING;
    if(PGobject->status != CONNECTION_OK){
        php_printf("进来了吗");
        for (;;)
        {
            switch (flag)
            {
                case PGRES_POLLING_OK:
                    break;
                case PGRES_POLLING_READING:
                    break;
                case PGRES_POLLING_WRITING:
                    php_printf("写");
                    break;
                default:
                    break;
            }

            flag = PQconnectPoll(PGobject->conn);
            if(flag == PGRES_POLLING_OK){
                php_printf("ok");
                PGobject->status = CONNECTION_OK;
                break;

            }
        }

    }
    /*

 */

    //mysql_client *client = event->socket->object;
    //success
    if (SwooleG.error == 0)
    {
        php_printf("来了33\n");

        //listen read event
        php_printf("eventfd:%d",event->fd);
        SwooleG.main_reactor->set(SwooleG.main_reactor, event->fd, PHP_SWOOLE_FD_POSTGRESQL | SW_EVENT_READ);
        php_printf("来了34\n");
        //connected
        event->socket->active = 1;
        php_printf("来了35\n");

        php_context *sw_current_context = swoole_get_property(PGobject->object, 0);

        zval *retval = NULL;
        zval return_value;
        ZVAL_RES(&return_value, zend_register_resource(PGobject->conn, le_link));

        int ret = coro_resume(sw_current_context, &return_value, &retval);
        //client->handshake = SW_MYSQL_HANDSHAKE_WAIT_REQUEST;
    }
    else
    {
        //client->connector.error_code = SwooleG.error;
        //client->connector.error_msg = strerror(SwooleG.error);
        //client->connector.error_length = strlen(client->connector.error_msg);
        //swoole_mysql_coro_onConnect(client TSRMLS_CC);
    }
    php_printf("来了36\n");
    return SW_OK;
}
static int swoole_pgsql_coro_onRead(swReactor *reactor, swEvent *event)
{
    PGresult *pgsql_result;
    PGobject *PGobject = (event->socket->object);
    pgsql_result_handle *pg_result;
    int error;
    php_printf("来了4");
    swWarn("到这了哦 加油！");
#if PHP_MAJOR_VERSION < 7
    TSRMLS_FETCH_FROM_CTX(sw_thread_ctx ? sw_thread_ctx : NULL);
#endif


    ExecStatusType status;
    pgsql_result = PQgetResult(PGobject->conn);
    //reactor->del(SwooleG.main_reactor, event->fd);
    /*
    if ((PGG(auto_reset_persistent) & 2) && PQstatus(pgsql) != CONNECTION_OK) {
        PQclear(pgsql_result);
        PQreset(pgsql);
        pgsql_result = PQexec(pgsql, query);
    }
    */

        if (pgsql_result) {
            status = PQresultStatus(pgsql_result);
        } else {
            status = (ExecStatusType) PQstatus(PGobject->conn);
        }

        switch (status) {
            case PGRES_EMPTY_QUERY:
            case PGRES_BAD_RESPONSE:
            case PGRES_NONFATAL_ERROR:
            case PGRES_FATAL_ERROR:
                swWarn("Query failed: [%s]",PGobject->conn);
                PQclear(pgsql_result);
                //RETURN_FALSE;
                break;
            case PGRES_COMMAND_OK: /* successful command that did not return rows */
            default:
                if (pgsql_result) {
                    pg_result = (pgsql_result_handle *) emalloc(sizeof(pgsql_result_handle));
                    pg_result->conn = PGobject->conn;
                    pg_result->result = pgsql_result;
                    pg_result->row = 0;
                    php_context *sw_current_context = swoole_get_property(PGobject->object, 0);

                    zval *retval = NULL;
                    zval return_value;
                    ZVAL_RES(&return_value, zend_register_resource(pg_result, le_result));
                    //zval *zv = &((zend_reference*)te)->val;


                    int ret = coro_resume(sw_current_context, &return_value,  &retval);
                    php_printf("dayuleingma : %d",ret);

                    if (error != 0)
                    {
                        swoole_php_fatal_error(E_WARNING, "swoole_event->onError[1]: socket error. Error: %s [%d]", strerror(error), error);
                    }

                    efree(event->socket->object);

                    event->socket->active = 0;

                    return SW_OK;
                    //client->handshake = SW_MYSQL_HANDSHAKE_WAIT_REQUEST;
                    //RETURN_RES(zend_register_resource(pg_result, le_result));
                } else {
                    PQclear(pgsql_result);
                    //RETURN_FALSE;
                }
                break;
            }

    return SW_OK;
}

static PHP_METHOD(swoole_postgresql_coro, pgQuery)
{
    zval *pgsql_link = NULL;
    zval *query;
    PGconn *pgsql;
    PGresult *pgsql_result;
    ExecStatusType status;
    pgsql_result_handle *pg_result;

    ZEND_PARSE_PARAMETERS_START(2,2)
        Z_PARAM_RESOURCE(pgsql_link)
        Z_PARAM_ZVAL(query)
    ZEND_PARSE_PARAMETERS_END();

    pgsql = (PGconn *)zend_fetch_resource(Z_RES_P(pgsql_link), "postgresql connection", le_link);
    int ret  = PQsendQuery(pgsql, Z_STRVAL_P(query));

    php_context *sw_current_context = swoole_get_property(getThis(), 0);
    if (!sw_current_context)
    {
        sw_current_context = emalloc(sizeof(php_context));
        swoole_set_property(getThis(), 0, sw_current_context);
    }
    sw_current_context->state = SW_CORO_CONTEXT_RUNNING;
    sw_current_context->onTimeout = NULL;
    #if PHP_MAJOR_VERSION < 7
    sw_current_context->coro_params = getThis();
    #else
    sw_current_context->coro_params = *getThis();
    #endif
    /*
        if (redis->timeout > 0)
        {
            php_swoole_check_timer((int) (redis->timeout * 1000));
            redis->timer = SwooleG.timer.add(&SwooleG.timer, (int) (redis->timeout * 1000), 0, sw_current_context, swoole_redis_coro_onTimeout);
        }*/
    coro_save(sw_current_context);
    coro_yield();
    /*pgsql_result = PQgetResult(pgsql);*/

/*
    if ((PGG(auto_reset_persistent) & 2) && PQstatus(pgsql) != CONNECTION_OK) {
        PQclear(pgsql_result);
        PQreset(pgsql);
        pgsql_result = PQexec(pgsql, query);
    }
    */

/*
    if (pgsql_result) {
        status = PQresultStatus(pgsql_result);
    } else {
        status = (ExecStatusType) PQstatus(pgsql);
    }

    switch (status) {
        case PGRES_EMPTY_QUERY:
        case PGRES_BAD_RESPONSE:
        case PGRES_NONFATAL_ERROR:
        case PGRES_FATAL_ERROR:
            swWarn("Query failed: [%s]",pgsql);
            PQclear(pgsql_result);
            RETURN_FALSE;
            break;
        case PGRES_COMMAND_OK: *//* successful command that did not return rows */
/*
        default:
            if (pgsql_result) {
                pg_result = (pgsql_result_handle *) emalloc(sizeof(pgsql_result_handle));
                pg_result->conn = pgsql;
                pg_result->result = pgsql_result;
                pg_result->row = 0;
                RETURN_RES(zend_register_resource(pg_result, le_result));
            } else {
                PQclear(pgsql_result);
                RETURN_FALSE;
            }
            break;
    }
*/
}


/* {{{ php_pgsql_result2array
 */
int php_pgsql_result2array(PGresult *pg_result, zval *ret_array, long result_type)
{
    zval row;
    char *field_name;
    size_t num_fields;
    int pg_numrows, pg_row;
    uint32_t i;
    assert(Z_TYPE_P(ret_array) == IS_ARRAY);

    if ((pg_numrows = PQntuples(pg_result)) <= 0) {
        return FAILURE;
    }
    for (pg_row = 0; pg_row < pg_numrows; pg_row++) {
        array_init(&row);
        for (i = 0, num_fields = PQnfields(pg_result); i < num_fields; i++) {
            field_name = PQfname(pg_result, i);
            if (PQgetisnull(pg_result, pg_row, i)) {
                if (result_type & PGSQL_ASSOC) {
                    add_assoc_null(&row, field_name);
                }
                if (result_type & PGSQL_NUM) {
                    add_next_index_null(&row);
                }
            } else {
                char *element = PQgetvalue(pg_result, pg_row, i);
                if (element) {
                    const size_t element_len = strlen(element);
                    if (result_type & PGSQL_ASSOC) {
                        add_assoc_stringl(&row, field_name, element, element_len);
                    }
                    if (result_type & PGSQL_NUM) {
                        add_next_index_stringl(&row, element, element_len);
                    }
                }
            }
        }
        add_index_zval(ret_array, pg_row, &row);
    }
    return SUCCESS;
}

static PHP_METHOD(swoole_postgresql_coro, pg_fetch_all)
{
    zval *result;
    PGresult *pgsql_result;
    pgsql_result_handle *pg_result;
    zend_long result_type = PGSQL_ASSOC;

    ZEND_PARSE_PARAMETERS_START(1,2)
        Z_PARAM_RESOURCE(result)
        Z_PARAM_OPTIONAL
        Z_PARAM_LONG(result_type)
    ZEND_PARSE_PARAMETERS_END();

    if ((pg_result = (pgsql_result_handle *)zend_fetch_resource(Z_RES_P(result), "PostgreSQL result", le_result)) == NULL) {
        RETURN_FALSE;
    }

    pgsql_result = pg_result->result;
    array_init(return_value);
    if (php_pgsql_result2array(pgsql_result, return_value, result_type) == FAILURE) {
        zval_dtor(return_value);
        RETURN_FALSE;
    }
}




/* {{{ _close_pgsql_link
 */
static void _close_pgsql_link(zend_resource *rsrc)
{
    PGconn *link = (PGconn *)rsrc->ptr;
    PGresult *res;

    while ((res = PQgetResult(link))) {
        PQclear(res);
    }
    PQfinish(link);
}

static void _free_result(zend_resource *rsrc)
{
    pgsql_result_handle *pg_result = (pgsql_result_handle *)rsrc->ptr;

    PQclear(pg_result->result);
    efree(pg_result);
}
static int swoole_pgsql_coro_close(zval *this, int fd)
{
    //SwooleG.main_reactor->del(SwooleG.main_reactor, fd);
    return 1;


}

static PHP_METHOD(swoole_postgresql_coro, __destruct)
{
    PGobject *PGobject = swoole_get_object(getThis());

    SwooleG.main_reactor->del(SwooleG.main_reactor, PGobject->fd);
    efree(PGobject);
    php_context *sw_current_context = swoole_get_property(getThis(), 0);
    efree(sw_current_context);

}
