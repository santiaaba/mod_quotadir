#include "mod_quotadir.h"
#include "mod_sql.h"

/* Utilizamos el groudid para almacenar el id de la suscripcion.
 * De esta forma todos los usuarios ftp que tengan el mismo groupid
 * es porque pertenecen a la misma suscripcion. Cada usuario tendra
 * su propio homedir asi que cada uno podra acceder solo al sitio
 * que le corresponda. Aun si pertenecen a la msiam suscripcion */

//main_server:	es una variable global que posee entre otras cosas las configuraciones
//session	parace ser una estructura que posee la session del momento

module quotadir_module;

/* Logging data */
static int quotadir_logfd = -1;
static char *quotadir_logname = NULL;

/* Quota logging routines */
static int quotadir_closelog(void) {
	/* sanity check */
	if (quotadir_logfd >= 0) {
		(void) close(quotadir_logfd);
	}

	quotadir_logfd = -1;
	quotadir_logname = NULL;

	return 0;
}

int quotadir_openlog(void) {
	int res = 0;
	int xerrno;

	quotadir_logname = get_param_ptr(main_server->conf, "QuotadirLog", FALSE);
	if (quotadir_logname == NULL) {
		return 0;
	}

	/* check for "none" */
	if (strcasecmp(quotadir_logname, "none") == 0) {
		quotadir_logname = NULL;
		return 0;
	}

	pr_signals_block();
	PRIVS_ROOT
	res = pr_log_openfile(quotadir_logname, &quotadir_logfd, PR_LOG_SYSTEM_MODE);
	xerrno = errno;
	PRIVS_RELINQUISH
	pr_signals_unblock();

	switch (res) {
		case -1:
			pr_log_pri(LOG_NOTICE, MOD_QUOTADIR_VERSION
			": unable to open QuotaLog '%s': %s", quotadir_logname, strerror(xerrno));
			break;

		case PR_LOG_WRITABLE_DIR:
			pr_log_pri(LOG_NOTICE, MOD_QUOTADIR_VERSION
			": unable to open QuotaLog '%s': %s", quotadir_logname,
			"World-writable directory");
			break;

		case PR_LOG_SYMLINK:
			pr_log_pri(LOG_NOTICE, MOD_QUOTADIR_VERSION
			": unable to open QuotaLog '%s': %s", quotadir_logname, "Symbolic link");
			break;
	}
	return res;
}

int quotadir_log(const char *fmt, ...) {
	va_list msg;
	int res;

	/* sanity check */
	if (!quotadir_logname)
		return 0;

	va_start(msg, fmt);
	res = pr_log_vwritefile(quotadir_logfd, MOD_QUOTADIR_VERSION, fmt, msg);
	va_end(msg);

	return res;
}

/* End Quota logging routines */

static cmd_rec *sqltab_cmd_create(pool *parent_pool, unsigned int argc, ...) {
	register unsigned int i = 0;
	pool *cmd_pool = NULL;
	cmd_rec *cmd = NULL;
	va_list argp;

	cmd_pool = make_sub_pool(parent_pool);
	cmd = (cmd_rec *) pcalloc(cmd_pool, sizeof(cmd_rec));
	cmd->pool = cmd_pool;

	cmd->argc = argc;
	cmd->argv = pcalloc(cmd->pool, argc * sizeof(void *));

	/* Hmmm... */
	cmd->tmp_pool = cmd->pool;

	va_start(argp, argc);
	for (i = 0; i < argc; i++) {
		cmd->argv[i] = va_arg(argp, char *);
	}
	va_end(argp);
	return cmd;
}

static int quotadir_sess_init(void) {

	quotadir_openlog();
	return 0;
}

static void quotadir_exit_ev(const void *event_data, void *user_data) {
	quotadir_closelog();
	return;
}

#if defined(PR_SHARED_MODULE)
static void quotadir_mod_unload_ev(const void *event_data, void *user_data) {
	if (strcmp("mod_quotatab.c", (const char *) event_data) == 0) {
		pr_event_unregister(&quotadir_module, NULL, NULL);

	quotadir_closelog();
	}
}
#endif

static void quotadir_restart_ev(const void *event_data, void *user_data) {
	return;
}


static int quotadir_init(void) {

	#if defined(PR_SHARED_MODULE)
	pr_event_register(&quotadir_module, "core.module-unload",
	quotadir_mod_unload_ev, NULL);
	#endif
	pr_event_register(&quotadir_module, "core.restart", quotadir_restart_ev,NULL);

	return 0;
}

MODRET set_quotadirtable(cmd_rec *cmd){
	/* Verifica que en el archivo de configuracion la entrada para
 	 * "QuotaDirLimitTable" sea correcta y lo agrega a la estructura de
 	 * configuracion del proftpd */
	char *user = "";
	char *host = "";
	char *pass= "";
	
	CHECK_CONF(cmd, CONF_ROOT|CONF_GLOBAL|CONF_VIRTUAL);

	/* verificamos que sean dos argumentos user@db password */
	if (cmd->argc != 3){
		CONF_ERROR(cmd, "Se debe especificar <host_db> <user> <password>");
	}
	(void) add_config_param_str(cmd->argv[0],3,host,user,pass);
	return PR_HANDLED(cmd);
}

MODRET set_quotadirlog(cmd_rec *cmd) {
	CHECK_ARGS(cmd, 1);
	CHECK_CONF(cmd, CONF_ROOT|CONF_VIRTUAL|CONF_GLOBAL);

	add_config_param_str(cmd->argv[0], 1, cmd->argv[1]);

	return PR_HANDLED(cmd);
}

MODRET check_quotadir(cmd_rec *cmd){
	/* Se ejecuta antes de permitir subir un archivo. Verifica que la cuota
 	 * del directorio no se exceda si el archivo es subido.
 	 * La cuota se almacena en mega bytes */

	pool *tmp_pool = NULL;
	cmd_rec *sql_cmd = NULL;
	modret_t *sql_res = NULL;
	cmdtable *sql_cmdtab = NULL;
	array_header *sql_data = NULL;

	/* Como obtengo el groupid???? */
	quotadir_log("Chequeando cuota usuario %s (%i)(%i)",session.user,session.uid,session.gid);

	char sql_query[200];
	int susc_id=0;

	sprintf(sql_query,"select quota,quota_used from web_suscription where id = %i",session.gid);

	/* Find the cmdtable for the sql_lookup command. */
	sql_cmdtab = pr_stash_get_symbol2(PR_SYM_HOOK, "sql_lookup", NULL, NULL, NULL);
	if (sql_cmdtab == NULL) {
		quotatab_log("error: unable to find SQL hook symbol 'sql_lookup'");
		destroy_pool(tmp_pool);
		return PR_HANDLED(cmd);
	}

	/* Prepare the SELECT query. */
	sql_cmd = sqltab_cmd_create(tmp_pool, 2, "sql_lookup", sql_query);

	/* Call the handler. */
	sql_res = pr_module_call(sql_cmdtab->m, sql_cmdtab->handler, sql_cmd);
	/* Check the results. */
	if (!sql_res || MODRET_ISERROR(sql_res)) {
		quotadir_log("error processing NamedQuery '%s'", sql_query);
		destroy_pool(tmp_pool);
		return PR_HANDLED(cmd);
	}

	sql_data = (array_header *) sql_res->data;
	char **values = (char **) sql_data->elts;
	quotadir_log("quota=%s; usado=%s",values[0],values[1]);
	
	destroy_pool(tmp_pool);
	return PR_HANDLED(cmd);
}

MODRET change_quotadir(cmd_rec *cmd){
	/* Se ejecuta luego de subir o eliminar un archivo. Se encarga de actualziar
	 * la tabla de la base de datos que contiene el espacio actualmente ocupado
	 * del directorio. La cuota se almacena en mega bytes */

	char sql[200];
	unsigned int used = 0;
	unsigned int susc_id = 0;

	/* Obtenemos lo actualmente utilizado. Ver si no viene en cmd_rec */
	/* Le sumamos lo que pesa el archivo subido */
	used++;
	/* Actualizamos la info en la abse de datos */
	sprintf(sql,"update web_suscription set used = %i where id=%i",susc_id,used);
	//if(mysql_query(db->con,query)){
		/* ERROR FATAL */
	//}
	return PR_HANDLED(cmd);
}

static conftable quotadir_conftab[] = {
	{ "QuotaDirLimitTable",	set_quotadirtable,	NULL },
	{ "QuotadirLog",	set_quotadirlog,	NULL },
	{ 0, NULL }
};

static cmdtable quotadir_cmdtab[] = {
	{ PRE_CMD,	C_STOR,	G_NONE,	check_quotadir,		FALSE,	FALSE },
	{ POST_CMD,	C_DELE,	G_NONE,	change_quotadir,	FALSE,	FALSE },
	{ POST_CMD,	C_RMD,	G_NONE,	change_quotadir,	FALSE,	FALSE },
	{ POST_CMD,	C_XRMD,	G_NONE,	change_quotadir,	FALSE,	FALSE },
	{ 0, NULL, NULL }
};

module quotadir_module = {
  /* Always NULL */
  NULL, NULL,
  /* Module API version */
  0x20,
  /* Module name */
  "quotadir",
  /* Module configuration directive table */
  quotadir_conftab,
  /* Module command handler table */
  quotadir_cmdtab,
  /* Module auth handler table */
  NULL,
  /* Module initialization */
  quotadir_init,
  /* Session initialization */
  quotadir_sess_init,
  /* Module version */
  MOD_QUOTADIR_VERSION
};
