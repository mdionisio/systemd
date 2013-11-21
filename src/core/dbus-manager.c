/*-*- Mode: C; c-basic-offset: 8; indent-tabs-mode: nil -*-*/

/***
  This file is part of systemd.

  Copyright 2010 Lennart Poettering

  systemd is free software; you can redistribute it and/or modify it
  under the terms of the GNU Lesser General Public License as published by
  the Free Software Foundation; either version 2.1 of the License, or
  (at your option) any later version.

  systemd is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public License
  along with systemd; If not, see <http://www.gnu.org/licenses/>.
***/

#include <errno.h>
#include <unistd.h>

#include "log.h"
#include "strv.h"
#include "build.h"
#include "install.h"
#include "selinux-access.h"
#include "watchdog.h"
#include "hwclock.h"
#include "path-util.h"
#include "virt.h"
#include "env-util.h"
#include "dbus.h"
#include "dbus-manager.h"
#include "dbus-unit.h"
#include "dbus-snapshot.h"
#include "dbus-client-track.h"
#include "dbus-execute.h"
#include "bus-errors.h"

static int property_get_version(
                sd_bus *bus,
                const char *path,
                const char *interface,
                const char *property,
                sd_bus_message *reply,
                void *userdata,
                sd_bus_error *error) {

        assert(bus);
        assert(reply);

        return sd_bus_message_append(reply, "s", PACKAGE_VERSION);
}

static int property_get_features(
                sd_bus *bus,
                const char *path,
                const char *interface,
                const char *property,
                sd_bus_message *reply,
                void *userdata,
                sd_bus_error *error) {

        assert(bus);
        assert(reply);

        return sd_bus_message_append(reply, "s", SYSTEMD_FEATURES);
}

static int property_get_virtualization(
                sd_bus *bus,
                const char *path,
                const char *interface,
                const char *property,
                sd_bus_message *reply,
                void *userdata,
                sd_bus_error *error) {

        const char *id = NULL;

        assert(bus);
        assert(reply);

        detect_virtualization(&id);

        return sd_bus_message_append(reply, "s", id);
}

static int property_get_tainted(
                sd_bus *bus,
                const char *path,
                const char *interface,
                const char *property,
                sd_bus_message *reply,
                void *userdata,
                sd_bus_error *error) {

        char buf[sizeof("split-usr:mtab-not-symlink:cgroups-missing:local-hwclock:")] = "", *e = buf;
        _cleanup_free_ char *p = NULL;
        Manager *m = userdata;

        assert(bus);
        assert(reply);
        assert(m);

        if (m->taint_usr)
                e = stpcpy(e, "split-usr:");

        if (readlink_malloc("/etc/mtab", &p) < 0)
                e = stpcpy(e, "mtab-not-symlink:");

        if (access("/proc/cgroups", F_OK) < 0)
                e = stpcpy(e, "cgroups-missing:");

        if (hwclock_is_localtime() > 0)
                e = stpcpy(e, "local-hwclock:");

        /* remove the last ':' */
        if (e != buf)
                e[-1] = 0;

        return sd_bus_message_append(reply, "s", buf);
}

static int property_get_log_target(
                sd_bus *bus,
                const char *path,
                const char *interface,
                const char *property,
                sd_bus_message *reply,
                void *userdata,
                sd_bus_error *error) {

        assert(bus);
        assert(reply);

        return sd_bus_message_append(reply, "s", log_target_to_string(log_get_target()));
}

static int property_set_log_target(
                sd_bus *bus,
                const char *path,
                const char *interface,
                const char *property,
                sd_bus_message *value,
                void *userdata,
                sd_bus_error *error) {

        const char *t;
        int r;

        assert(bus);
        assert(value);

        r = sd_bus_message_read(value, "s", &t);
        if (r < 0)
                return r;

        return log_set_target_from_string(t);
}

static int property_get_log_level(
                sd_bus *bus,
                const char *path,
                const char *interface,
                const char *property,
                sd_bus_message *reply,
                void *userdata,
                sd_bus_error *error) {

        _cleanup_free_ char *t = NULL;
        int r;

        assert(bus);
        assert(reply);

        r = log_level_to_string_alloc(log_get_max_level(), &t);
        if (r < 0)
                return r;

        return sd_bus_message_append(reply, "s", t);
}

static int property_set_log_level(
                sd_bus *bus,
                const char *path,
                const char *interface,
                const char *property,
                sd_bus_message *value,
                void *userdata,
                sd_bus_error *error) {

        const char *t;
        int r;

        assert(bus);
        assert(value);

        r = sd_bus_message_read(value, "s", &t);
        if (r < 0)
                return r;

        return log_set_max_level_from_string(t);
}

static int property_get_n_names(
                sd_bus *bus,
                const char *path,
                const char *interface,
                const char *property,
                sd_bus_message *reply,
                void *userdata,
                sd_bus_error *error) {

        Manager *m = userdata;

        assert(bus);
        assert(reply);
        assert(m);

        return sd_bus_message_append(reply, "u", (uint32_t) hashmap_size(m->units));
}

static int property_get_n_jobs(
                sd_bus *bus,
                const char *path,
                const char *interface,
                const char *property,
                sd_bus_message *reply,
                void *userdata,
                sd_bus_error *error) {

        Manager *m = userdata;

        assert(bus);
        assert(reply);
        assert(m);

        return sd_bus_message_append(reply, "u", (uint32_t) hashmap_size(m->jobs));
}

static int property_get_progress(
                sd_bus *bus,
                const char *path,
                const char *interface,
                const char *property,
                sd_bus_message *reply,
                void *userdata,
                sd_bus_error *error) {

        Manager *m = userdata;
        double d;

        assert(bus);
        assert(reply);
        assert(m);

        if (dual_timestamp_is_set(&m->finish_timestamp))
                d = 1.0;
        else
                d = 1.0 - ((double) hashmap_size(m->jobs) / (double) m->n_installed_jobs);

        return sd_bus_message_append(reply, "d", d);
}

static int property_set_runtime_watchdog(
                sd_bus *bus,
                const char *path,
                const char *interface,
                const char *property,
                sd_bus_message *value,
                void *userdata,
                sd_bus_error *error) {

        usec_t *t = userdata;
        int r;

        assert(bus);
        assert(value);

        assert_cc(sizeof(usec_t) == sizeof(uint64_t));

        r = sd_bus_message_read(value, "t", t);
        if (r < 0)
                return r;

        return watchdog_set_timeout(t);
}

static int method_get_unit(sd_bus *bus, sd_bus_message *message, void *userdata, sd_bus_error *error) {
        _cleanup_free_ char *path = NULL;
        Manager *m = userdata;
        const char *name;
        Unit *u;
        int r;

        assert(bus);
        assert(message);
        assert(m);

        r = sd_bus_message_read(message, "s", &name);
        if (r < 0)
                return r;

        u = manager_get_unit(m, name);
        if (!u)
                return sd_bus_error_setf(error, BUS_ERROR_NO_SUCH_UNIT, "Unit %s not loaded.", name);

        r = selinux_unit_access_check(u, bus, message, "status", error);
        if (r < 0)
                return r;

        path = unit_dbus_path(u);
        if (!path)
                return -ENOMEM;

        return sd_bus_reply_method_return(message, "o", path);
}

static int method_get_unit_by_pid(sd_bus *bus, sd_bus_message *message, void *userdata, sd_bus_error *error) {
        _cleanup_free_ char *path = NULL;
        Manager *m = userdata;
        pid_t pid;
        Unit *u;
        int r;

        assert(bus);
        assert(message);
        assert(m);

        assert_cc(sizeof(pid_t) == sizeof(uint32_t));

        r = sd_bus_message_read(message, "u", &pid);
        if (r < 0)
                return r;

        if (pid == 0) {
                r = sd_bus_get_owner_pid(bus, sd_bus_message_get_sender(message), &pid);
                if (r < 0)
                        return r;
        }

        u = manager_get_unit_by_pid(m, pid);
        if (!u)
                return sd_bus_error_setf(error, BUS_ERROR_NO_UNIT_FOR_PID, "PID %u does not belong to any loaded unit.", pid);

        r = selinux_unit_access_check(u, bus, message, "status", error);
        if (r < 0)
                return r;

        path = unit_dbus_path(u);
        if (!path)
                return -ENOMEM;

        return sd_bus_reply_method_return(message, "o", path);
}

static int method_load_unit(sd_bus *bus, sd_bus_message *message, void *userdata, sd_bus_error *error) {
        _cleanup_free_ char *path = NULL;
        Manager *m = userdata;
        const char *name;
        Unit *u;
        int r;

        assert(bus);
        assert(message);
        assert(m);

        r = sd_bus_message_read(message, "s", &name);
        if (r < 0)
                return r;

        r = manager_load_unit(m, name, NULL, error, &u);
        if (r < 0)
                return r;

        r = selinux_unit_access_check(u, bus, message, "status", error);
        if (r < 0)
                return r;

        path = unit_dbus_path(u);
        if (!path)
                return -ENOMEM;

        return sd_bus_reply_method_return(message, "o", path);
}

static int method_start_unit_generic(sd_bus *bus, sd_bus_message *message, Manager *m, JobType job_type, bool reload_if_possible, sd_bus_error *error) {
        const char *name;
        Unit *u;
        int r;

        assert(bus);
        assert(message);
        assert(m);

        r = sd_bus_message_read(message, "s", &name);
        if (r < 0)
                return r;

        r = manager_load_unit(m, name, NULL, error, &u);
        if (r < 0)
                return r;

        return bus_unit_method_start_generic(bus, message, u, job_type, reload_if_possible, error);
}

static int method_start_unit(sd_bus *bus, sd_bus_message *message, void *userdata, sd_bus_error *error) {
        return method_start_unit_generic(bus, message, userdata, JOB_START, false, error);
}

static int method_stop_unit(sd_bus *bus, sd_bus_message *message, void *userdata, sd_bus_error *error) {
        return method_start_unit_generic(bus, message, userdata, JOB_STOP, false, error);
}

static int method_reload_unit(sd_bus *bus, sd_bus_message *message, void *userdata, sd_bus_error *error) {
        return method_start_unit_generic(bus, message, userdata, JOB_RELOAD, false, error);
}

static int method_restart_unit(sd_bus *bus, sd_bus_message *message, void *userdata, sd_bus_error *error) {
        return method_start_unit_generic(bus, message, userdata, JOB_RESTART, false, error);
}

static int method_try_restart_unit(sd_bus *bus, sd_bus_message *message, void *userdata, sd_bus_error *error) {
        return method_start_unit_generic(bus, message, userdata, JOB_TRY_RESTART, false, error);
}

static int method_reload_or_restart_unit(sd_bus *bus, sd_bus_message *message, void *userdata, sd_bus_error *error) {
        return method_start_unit_generic(bus, message, userdata, JOB_RESTART, true, error);
}

static int method_reload_or_try_restart_unit(sd_bus *bus, sd_bus_message *message, void *userdata, sd_bus_error *error) {
        return method_start_unit_generic(bus, message, userdata, JOB_TRY_RESTART, true, error);
}

static int method_start_unit_replace(sd_bus *bus, sd_bus_message *message, void *userdata, sd_bus_error *error) {
        Manager *m = userdata;
        const char *old_name;
        Unit *u;
        int r;

        assert(bus);
        assert(message);
        assert(m);

        r = sd_bus_message_read(message, "s", &old_name);
        if (r < 0)
                return r;

        u = manager_get_unit(m, old_name);
        if (!u || !u->job || u->job->type != JOB_START)
                return sd_bus_error_setf(error, BUS_ERROR_NO_SUCH_JOB, "No job queued for unit %s", old_name);

        return method_start_unit_generic(bus, message, m, JOB_START, false, error);
}

static int method_kill_unit(sd_bus *bus, sd_bus_message *message, void *userdata, sd_bus_error *error) {
        Manager *m = userdata;
        const char *name;
        Unit *u;
        int r;

        assert(bus);
        assert(message);
        assert(m);

        r = sd_bus_message_read(message, "s", &name);
        if (r < 0)
                return r;

        u = manager_get_unit(m, name);
        if (!u)
                return sd_bus_error_setf(error, BUS_ERROR_NO_SUCH_UNIT, "Unit %s is not loaded.", name);

        return bus_unit_method_kill(bus, message, u, error);
}

static int method_reset_failed_unit(sd_bus *bus, sd_bus_message *message, void *userdata, sd_bus_error *error) {
        Manager *m = userdata;
        const char *name;
        Unit *u;
        int r;

        assert(bus);
        assert(message);
        assert(m);

        r = sd_bus_message_read(message, "s", &name);
        if (r < 0)
                return r;

        u = manager_get_unit(m, name);
        if (!u)
                return sd_bus_error_setf(error, BUS_ERROR_NO_SUCH_UNIT, "Unit %s is not loaded.", name);

        return bus_unit_method_reset_failed(bus, message, u, error);
}

static int method_set_unit_properties(sd_bus *bus, sd_bus_message *message, void *userdata, sd_bus_error *error) {
        Manager *m = userdata;
        const char *name;
        Unit *u;
        int r;

        assert(bus);
        assert(message);
        assert(m);

        r = sd_bus_message_read(message, "s", &name);
        if (r < 0)
                return r;

        u = manager_get_unit(m, name);
        if (!u)
                return sd_bus_error_setf(error, BUS_ERROR_NO_SUCH_UNIT, "Unit %s is not loaded.", name);

        return bus_unit_method_set_properties(bus, message, u, error);
}

static int method_start_transient_unit(sd_bus *bus, sd_bus_message *message, void *userdata, sd_bus_error *error) {
        const char *name, *smode;
        Manager *m = userdata;
        JobMode mode;
        UnitType t;
        Unit *u;
        int r;

        assert(bus);
        assert(message);
        assert(m);

        r = sd_bus_message_read(message, "ss", &name, &smode);
        if (r < 0)
                return r;

        t = unit_name_to_type(name);
        if (t < 0)
                return sd_bus_error_setf(error, SD_BUS_ERROR_INVALID_ARGS, "Invalid unit type.");

        if (!unit_vtable[t]->can_transient)
                return sd_bus_error_setf(error, SD_BUS_ERROR_INVALID_ARGS, "Unit type %s does not support transient units.");

        mode = job_mode_from_string(smode);
        if (mode < 0)
                return sd_bus_error_setf(error, SD_BUS_ERROR_INVALID_ARGS, "Job mode %s is invalid.", smode);

        r = selinux_access_check(bus, message, "start", error);
        if (r < 0)
                return r;

        r = manager_load_unit(m, name, NULL, error, &u);
        if (r < 0)
                return r;

        if (u->load_state != UNIT_NOT_FOUND || set_size(u->dependencies[UNIT_REFERENCED_BY]) > 0)
                return sd_bus_error_setf(error, BUS_ERROR_UNIT_EXISTS, "Unit %s already exists.", name);

        /* OK, the unit failed to load and is unreferenced, now let's
         * fill in the transient data instead */
        r = unit_make_transient(u);
        if (r < 0)
                return r;

        /* Set our properties */
        r = bus_unit_set_properties(u, message, UNIT_RUNTIME, false, error);
        if (r < 0)
                return r;

        /* And load this stub fully */
        r = unit_load(u);
        if (r < 0)
                return r;

        manager_dispatch_load_queue(m);

        /* Finally, start it */
        return bus_unit_queue_job(bus, message, u, JOB_START, mode, false, error);
}

static int method_get_job(sd_bus *bus, sd_bus_message *message, void *userdata, sd_bus_error *error) {
        _cleanup_free_ char *path = NULL;
        Manager *m = userdata;
        uint32_t id;
        Job *j;
        int r;

        assert(bus);
        assert(message);
        assert(m);

        r = sd_bus_message_read(message, "u", &id);
        if (r < 0)
                return r;

        j = manager_get_job(m, id);
        if (!j)
                return sd_bus_error_setf(error, BUS_ERROR_NO_SUCH_JOB, "Job %u does not exist.", (unsigned) id);

        r = selinux_unit_access_check(j->unit, bus, message, "status", error);
        if (r < 0)
                return r;

        path = job_dbus_path(j);
        if (!path)
                return -ENOMEM;

        return sd_bus_reply_method_return(message, "o", path);
}

static int method_cancel_job(sd_bus *bus, sd_bus_message *message, void *userdata, sd_bus_error *error) {
        Manager *m = userdata;
        uint32_t id;
        Job *j;
        int r;

        assert(bus);
        assert(message);
        assert(m);

        r = sd_bus_message_read(message, "u", &id);
        if (r < 0)
                return r;

        j = manager_get_job(m, id);
        if (!j)
                return sd_bus_error_setf(error, BUS_ERROR_NO_SUCH_JOB, "Job %u does not exist.", (unsigned) id);

        r = selinux_unit_access_check(j->unit, bus, message, "stop", error);
        if (r < 0)
                return r;

        job_finish_and_invalidate(j, JOB_CANCELED, true);

        return sd_bus_reply_method_return(message, NULL);
}

static int method_clear_jobs(sd_bus *bus, sd_bus_message *message, void *userdata, sd_bus_error *error) {
        Manager *m = userdata;
        int r;

        assert(bus);
        assert(message);
        assert(m);

        r = selinux_access_check(bus, message, "reboot", error);
        if (r < 0)
                return r;

        manager_clear_jobs(m);

        return sd_bus_reply_method_return(message, NULL);
}

static int method_reset_failed(sd_bus *bus, sd_bus_message *message, void *userdata, sd_bus_error *error) {
        Manager *m = userdata;
        int r;

        assert(bus);
        assert(message);
        assert(m);

        r = selinux_access_check(bus, message, "reload", error);
        if (r < 0)
                return r;

        manager_reset_failed(m);

        return sd_bus_reply_method_return(message, NULL);
}

static int method_list_units(sd_bus *bus, sd_bus_message *message, void *userdata, sd_bus_error *error) {
        _cleanup_bus_message_unref_ sd_bus_message *reply = NULL;
        Manager *m = userdata;
        const char *k;
        Iterator i;
        Unit *u;
        int r;

        assert(bus);
        assert(message);
        assert(m);

        r = selinux_access_check(bus, message, "status", error);
        if (r < 0)
                return r;

        r = sd_bus_message_new_method_return(message, &reply);
        if (r < 0)
                return r;

        r = sd_bus_message_open_container(reply, 'a', "(ssssssouso)");
        if (r < 0)
                return r;

        HASHMAP_FOREACH_KEY(u, k, m->units, i) {
                _cleanup_free_ char *unit_path = NULL, *job_path = NULL;
                Unit *following;

                if (k != u->id)
                        continue;

                following = unit_following(u);

                unit_path = unit_dbus_path(u);
                if (!unit_path)
                        return -ENOMEM;

                if (u->job) {
                        job_path = job_dbus_path(u->job);
                        if (!job_path)
                                return -ENOMEM;
                }

                r = sd_bus_message_append(
                                reply, "(ssssssouso)",
                                u->id,
                                unit_description(u),
                                unit_load_state_to_string(u->load_state),
                                unit_active_state_to_string(unit_active_state(u)),
                                unit_sub_state_to_string(u),
                                following ? following->id : "",
                                unit_path,
                                u->job ? u->job->id : 0,
                                u->job ? job_type_to_string(u->job->type) : "",
                                job_path ? job_path : "/");
                if (r < 0)
                        return r;
        }

        r = sd_bus_message_close_container(reply);
        if (r < 0)
                return r;

        return sd_bus_send(bus, reply, NULL);
}

static int method_list_jobs(sd_bus *bus, sd_bus_message *message, void *userdata, sd_bus_error *error) {
        _cleanup_bus_message_unref_ sd_bus_message *reply = NULL;
        Manager *m = userdata;
        Iterator i;
        Job *j;
        int r;

        assert(bus);
        assert(message);
        assert(m);

        r = selinux_access_check(bus, message, "status", error);
        if (r < 0)
                return r;

        r = sd_bus_message_new_method_return(message, &reply);
        if (r < 0)
                return r;

        r = sd_bus_message_open_container(reply, 'a', "(usssoo)");
        if (r < 0)
                return r;

        HASHMAP_FOREACH(j, m->jobs, i) {
                _cleanup_free_ char *unit_path = NULL, *job_path = NULL;

                job_path = job_dbus_path(j);
                if (!job_path)
                        return -ENOMEM;

                unit_path = unit_dbus_path(j->unit);
                if (!unit_path)
                        return -ENOMEM;

                r = sd_bus_message_append(
                                reply, "(usssoo)",
                                j->id,
                                j->unit->id,
                                job_type_to_string(j->type),
                                job_state_to_string(j->state),
                                job_path,
                                unit_path);
                if (r < 0)
                        return r;
        }

        r = sd_bus_message_close_container(reply);
        if (r < 0)
                return r;

        return sd_bus_send(bus, reply, NULL);
}

static int method_subscribe(sd_bus *bus, sd_bus_message *message, void *userdata, sd_bus_error *error) {
        Manager *m = userdata;
        int r;

        assert(bus);
        assert(message);
        assert(m);

        r = selinux_access_check(bus, message, "status", error);
        if (r < 0)
                return r;

        r = bus_client_track(&m->subscribed, bus, sd_bus_message_get_sender(message));
        if (r < 0)
                return r;
        if (r == 0)
                return sd_bus_error_setf(error, BUS_ERROR_ALREADY_SUBSCRIBED, "Client is already subscribed.");

        return sd_bus_reply_method_return(message, NULL);
}

static int method_unsubscribe(sd_bus *bus, sd_bus_message *message, void *userdata, sd_bus_error *error) {
        Manager *m = userdata;
        int r;

        assert(bus);
        assert(message);
        assert(m);

        r = selinux_access_check(bus, message, "status", error);
        if (r < 0)
                return r;

        r = bus_client_untrack(m->subscribed, bus, sd_bus_message_get_sender(message));
        if (r < 0)
                return r;
        if (r == 0)
                return sd_bus_error_setf(error, BUS_ERROR_NOT_SUBSCRIBED, "Client is not subscribed.");

        return sd_bus_reply_method_return(message, NULL);
}

static int method_dump(sd_bus *bus, sd_bus_message *message, void *userdata, sd_bus_error *error) {
        _cleanup_free_ char *dump = NULL;
        _cleanup_fclose_ FILE *f = NULL;
        Manager *m = userdata;
        size_t size;
        int r;

        assert(bus);
        assert(message);
        assert(m);

        r = selinux_access_check(bus, message, "status", error);
        if (r < 0)
                return r;

        f = open_memstream(&dump, &size);
        if (!f)
                return -ENOMEM;

        manager_dump_units(m, f, NULL);
        manager_dump_jobs(m, f, NULL);

        fflush(f);

        if (ferror(f))
                return -ENOMEM;

        return sd_bus_reply_method_return(message, "s", dump);
}

static int method_create_snapshot(sd_bus *bus, sd_bus_message *message, void *userdata, sd_bus_error *error) {
        _cleanup_free_ char *path = NULL;
        Manager *m = userdata;
        const char *name;
        int cleanup;
        Snapshot *s;
        int r;

        assert(bus);
        assert(message);
        assert(m);

        r = selinux_access_check(bus, message, "start", error);
        if (r < 0)
                return r;

        r = sd_bus_message_read(message, "sb", &name, &cleanup);
        if (r < 0)
                return r;

        if (isempty(name))
                name = NULL;

        r = snapshot_create(m, name, cleanup, error, &s);
        if (r < 0)
                return r;

        path = unit_dbus_path(UNIT(s));
        if (!path)
                return -ENOMEM;

        return sd_bus_reply_method_return(message, "o", path);
}

static int method_remove_snapshot(sd_bus *bus, sd_bus_message *message, void *userdata, sd_bus_error *error) {
        Manager *m = userdata;
        const char *name;
        Unit *u;
        int r;

        assert(bus);
        assert(message);
        assert(m);

        r = selinux_access_check(bus, message, "stop", error);
        if (r < 0)
                return r;

        r = sd_bus_message_read(message, "s", &name);
        if (r < 0)
                return r;

        u = manager_get_unit(m, name);
        if (!u)
                return sd_bus_error_setf(error, BUS_ERROR_NO_SUCH_UNIT, "Unit %s does not exist.", name);

        if (u->type != UNIT_SNAPSHOT)
                return sd_bus_error_setf(error, BUS_ERROR_NO_SUCH_UNIT, "Unit %s is not a snapshot", name);

        return bus_snapshot_method_remove(bus, message, u, error);
}

static int method_reload(sd_bus *bus, sd_bus_message *message, void *userdata, sd_bus_error *error) {
        Manager *m = userdata;
        int r;

        assert(bus);
        assert(message);
        assert(m);

        r = selinux_access_check(bus, message, "reload", error);
        if (r < 0)
                return r;

        /* Instead of sending the reply back right away, we just
         * remember that we need to and then send it after the reload
         * is finished. That way the caller knows when the reload
         * finished. */

        assert(!m->queued_message);
        r = sd_bus_message_new_method_return(message, &m->queued_message);
        if (r < 0)
                return r;

        m->queued_message_bus = sd_bus_ref(bus);
        m->exit_code = MANAGER_RELOAD;

        return 1;
}

static int method_reexecute(sd_bus *bus, sd_bus_message *message, void *userdata, sd_bus_error *error) {
        Manager *m = userdata;
        int r;

        assert(bus);
        assert(message);
        assert(m);

        r = selinux_access_check(bus, message, "reload", error);
        if (r < 0)
                return r;

        /* We don't send a reply back here, the client should
         * just wait for us disconnecting. */

        m->exit_code = MANAGER_REEXECUTE;
        return 1;
}

static int method_exit(sd_bus *bus, sd_bus_message *message, void *userdata, sd_bus_error *error) {
        Manager *m = userdata;
        int r;

        assert(bus);
        assert(message);
        assert(m);

        r = selinux_access_check(bus, message, "halt", error);
        if (r < 0)
                return r;

        if (m->running_as == SYSTEMD_SYSTEM)
                return sd_bus_error_setf(error, SD_BUS_ERROR_NOT_SUPPORTED, "Exit is only supported for user service managers.");

        m->exit_code = MANAGER_EXIT;

        return sd_bus_reply_method_return(message, NULL);
}

static int method_reboot(sd_bus *bus, sd_bus_message *message, void *userdata, sd_bus_error *error) {
        Manager *m = userdata;
        int r;

        assert(bus);
        assert(message);
        assert(m);

        r = selinux_access_check(bus, message, "reboot", error);
        if (r < 0)
                return r;

        if (m->running_as != SYSTEMD_SYSTEM)
                return sd_bus_error_setf(error, SD_BUS_ERROR_NOT_SUPPORTED, "Reboot is only supported for system managers.");

        m->exit_code = MANAGER_REBOOT;

        return sd_bus_reply_method_return(message, NULL);
}


static int method_poweroff(sd_bus *bus, sd_bus_message *message, void *userdata, sd_bus_error *error) {
        Manager *m = userdata;
        int r;

        assert(bus);
        assert(message);
        assert(m);

        r = selinux_access_check(bus, message, "halt", error);
        if (r < 0)
                return r;

        if (m->running_as != SYSTEMD_SYSTEM)
                return sd_bus_error_setf(error, SD_BUS_ERROR_NOT_SUPPORTED, "Powering off is only supported for system managers.");

        m->exit_code = MANAGER_POWEROFF;

        return sd_bus_reply_method_return(message, NULL);
}

static int method_halt(sd_bus *bus, sd_bus_message *message, void *userdata, sd_bus_error *error) {
        Manager *m = userdata;
        int r;

        assert(bus);
        assert(message);
        assert(m);

        r = selinux_access_check(bus, message, "halt", error);
        if (r < 0)
                return r;

        if (m->running_as != SYSTEMD_SYSTEM)
                return sd_bus_error_setf(error, SD_BUS_ERROR_NOT_SUPPORTED, "Halt is only supported for system managers.");

        m->exit_code = MANAGER_HALT;

        return sd_bus_reply_method_return(message, NULL);
}

static int method_kexec(sd_bus *bus, sd_bus_message *message, void *userdata, sd_bus_error *error) {
        Manager *m = userdata;
        int r;

        assert(bus);
        assert(message);
        assert(m);

        r = selinux_access_check(bus, message, "reboot", error);
        if (r < 0)
                return r;

        if (m->running_as != SYSTEMD_SYSTEM)
                return sd_bus_error_setf(error, SD_BUS_ERROR_NOT_SUPPORTED, "KExec is only supported for system managers.");

        m->exit_code = MANAGER_KEXEC;

        return sd_bus_reply_method_return(message, NULL);
}

static int method_switch_root(sd_bus *bus, sd_bus_message *message, void *userdata, sd_bus_error *error) {
        char *ri = NULL, *rt = NULL;
        const char *root, *init;
        Manager *m = userdata;
        int r;

        assert(bus);
        assert(message);
        assert(m);

        r = selinux_access_check(bus, message, "reboot", error);
        if (r < 0)
                return r;

        if (m->running_as != SYSTEMD_SYSTEM)
                return sd_bus_error_setf(error, SD_BUS_ERROR_NOT_SUPPORTED, "KExec is only supported for system managers.");

        r = sd_bus_message_read(message, "ss", &root, &init);
        if (r < 0)
                return r;

        if (path_equal(root, "/") || !path_is_absolute(root))
                return sd_bus_error_setf(error, SD_BUS_ERROR_INVALID_ARGS, "Invalid switch root path %s", root);

        /* Safety check */
        if (isempty(init)) {
                if (! path_is_os_tree(root))
                        return sd_bus_error_setf(error, SD_BUS_ERROR_INVALID_ARGS, "Specified switch root path %s does not seem to be an OS tree. /etc/os-release is missing.", root);
        } else {
                _cleanup_free_ char *p = NULL;

                if (!path_is_absolute(init))
                        return sd_bus_error_setf(error, SD_BUS_ERROR_INVALID_ARGS, "Invalid init path %s", init);

                p = strappend(root, init);
                if (!p)
                        return -ENOMEM;

                if (access(p, X_OK) < 0)
                        return sd_bus_error_setf(error, SD_BUS_ERROR_INVALID_ARGS, "Specified init binary %s does not exist.", p);
        }

        rt = strdup(root);
        if (!rt)
                return -ENOMEM;

        if (!isempty(init)) {
                ri = strdup(init);
                if (!ri) {
                        free(ri);
                        return -ENOMEM;
                }
        }

        free(m->switch_root);
        m->switch_root = rt;

        free(m->switch_root_init);
        m->switch_root_init = ri;

        return sd_bus_reply_method_return(message, NULL);
}

static int method_set_environment(sd_bus *bus, sd_bus_message *message, void *userdata, sd_bus_error *error) {
        _cleanup_strv_free_ char **plus = NULL;
        Manager *m = userdata;
        int r;

        assert(bus);
        assert(message);
        assert(m);

        r = selinux_access_check(bus, message, "reload", error);
        if (r < 0)
                return r;

        r = sd_bus_message_read_strv(message, &plus);
        if (r < 0)
                return r;
        if (!strv_env_is_valid(plus))
                return sd_bus_error_setf(error, SD_BUS_ERROR_INVALID_ARGS, "Invalid environment assignments");

        r = manager_environment_add(m, NULL, plus);
        if (r < 0)
                return r;

        return sd_bus_reply_method_return(message, NULL);
}

static int method_unset_environment(sd_bus *bus, sd_bus_message *message, void *userdata, sd_bus_error *error) {
        _cleanup_strv_free_ char **minus = NULL;
        Manager *m = userdata;
        int r;

        assert(bus);
        assert(message);
        assert(m);

        r = selinux_access_check(bus, message, "reload", error);
        if (r < 0)
                return r;

        r = sd_bus_message_read_strv(message, &minus);
        if (r < 0)
                return r;

        if (!strv_env_name_or_assignment_is_valid(minus))
                return sd_bus_error_setf(error, SD_BUS_ERROR_INVALID_ARGS, "Invalid environment variable names or assignments");

        r = manager_environment_add(m, minus, NULL);
        if (r < 0)
                return r;

        return sd_bus_reply_method_return(message, NULL);
}

static int method_unset_and_set_environment(sd_bus *bus, sd_bus_message *message, void *userdata, sd_bus_error *error) {
        _cleanup_strv_free_ char **minus = NULL, **plus = NULL;
        Manager *m = userdata;
        int r;

        assert(bus);
        assert(message);
        assert(m);

        r = selinux_access_check(bus, message, "reload", error);
        if (r < 0)
                return r;

        r = sd_bus_message_read_strv(message, &plus);
        if (r < 0)
                return r;

        r = sd_bus_message_read_strv(message, &minus);
        if (r < 0)
                return r;

        if (!strv_env_is_valid(plus))
                return sd_bus_error_setf(error, SD_BUS_ERROR_INVALID_ARGS, "Invalid environment assignments");
        if (!strv_env_name_or_assignment_is_valid(minus))
                return sd_bus_error_setf(error, SD_BUS_ERROR_INVALID_ARGS, "Invalid environment variable names or assignments");

        r = manager_environment_add(m, minus, plus);
        if (r < 0)
                return r;

        return sd_bus_reply_method_return(message, NULL);
}

static int method_list_unit_files(sd_bus *bus, sd_bus_message *message, void *userdata, sd_bus_error *error) {
        _cleanup_bus_message_unref_ sd_bus_message *reply = NULL;
        Manager *m = userdata;
        UnitFileList *item;
        Hashmap *h;
        Iterator i;
        int r;

        assert(bus);
        assert(message);
        assert(m);

        r = selinux_access_check(bus, message, "status", error);
        if (r < 0)
                return r;

        r = sd_bus_message_new_method_return(message, &reply);
        if (r < 0)
                return r;

        h = hashmap_new(string_hash_func, string_compare_func);
        if (!h)
                return -ENOMEM;

        r = unit_file_get_list(m->running_as == SYSTEMD_SYSTEM ? UNIT_FILE_SYSTEM : UNIT_FILE_USER, NULL, h);
        if (r < 0)
                goto fail;

        r = sd_bus_message_open_container(reply, 'a', "(ss)");
        if (r < 0)
                goto fail;

        HASHMAP_FOREACH(item, h, i) {

                r = sd_bus_message_append(reply, "(ss)", item->path, unit_file_state_to_string(item->state));
                if (r < 0)
                        goto fail;
        }

        unit_file_list_free(h);

        r = sd_bus_message_close_container(reply);
        if (r < 0)
                return r;

        return sd_bus_send(bus, reply, NULL);

fail:
        unit_file_list_free(h);
        return r;
}

static int method_get_unit_file_state(sd_bus *bus, sd_bus_message *message, void *userdata, sd_bus_error *error) {
        Manager *m = userdata;
        const char *name;
        UnitFileState state;
        UnitFileScope scope;
        int r;

        assert(bus);
        assert(message);
        assert(m);

        r = selinux_access_check(bus, message, "status", error);
        if (r < 0)
                return r;

        r = sd_bus_message_read(message, "s", &name);
        if (r < 0)
                return r;

        scope = m->running_as == SYSTEMD_SYSTEM ? UNIT_FILE_SYSTEM : UNIT_FILE_USER;

        state = unit_file_get_state(scope, NULL, name);
        if (state < 0)
                return state;

        return sd_bus_reply_method_return(message, "s", unit_file_state_to_string(state));
}

static int method_get_default_target(sd_bus *bus, sd_bus_message *message, void *userdata, sd_bus_error *error) {
        _cleanup_free_ char *default_target = NULL;
        Manager *m = userdata;
        UnitFileScope scope;
        int r;

        assert(bus);
        assert(message);
        assert(m);

        r = selinux_access_check(bus, message, "status", error);
        if (r < 0)
                return r;

        scope = m->running_as == SYSTEMD_SYSTEM ? UNIT_FILE_SYSTEM : UNIT_FILE_USER;

        r = unit_file_get_default(scope, NULL, &default_target);
        if (r < 0)
                return r;

        return sd_bus_reply_method_return(message, "s", default_target);
}

static int send_unit_files_changed(sd_bus *bus, const char *destination, void *userdata) {
        _cleanup_bus_message_unref_ sd_bus_message *message = NULL;
        int r;

        assert(bus);

        r = sd_bus_message_new_signal(bus, "/org/freedesktop/systemd1", "org.freedesktop.systemd1.Manager", "UnitFilesChanged", &message);
        if (r < 0)
                return r;

        return sd_bus_send_to(bus, message, destination, NULL);
}

static int reply_unit_file_changes_and_free(
                Manager *m,
                sd_bus *bus,
                sd_bus_message *message,
                int carries_install_info,
                UnitFileChange *changes,
                unsigned n_changes) {

        _cleanup_bus_message_unref_ sd_bus_message *reply = NULL;
        unsigned i;
        int r;

        if (n_changes > 0)
                bus_manager_foreach_client(m, send_unit_files_changed, NULL);

        r = sd_bus_message_new_method_return(message, &reply);
        if (r < 0)
                goto fail;

        if (carries_install_info >= 0) {
                r = sd_bus_message_append(reply, "b", carries_install_info);
                if (r < 0)
                        goto fail;
        }

        r = sd_bus_message_open_container(reply, 'a', "(sss)");
        if (r < 0)
                goto fail;

        for (i = 0; i < n_changes; i++) {
                r = sd_bus_message_append(
                                reply, "(sss)",
                                unit_file_change_type_to_string(changes[i].type),
                                changes[i].path,
                                changes[i].source);
                if (r < 0)
                        goto fail;
        }

        r = sd_bus_message_close_container(reply);
        if (r < 0)
                goto fail;

        return sd_bus_send(bus, reply, NULL);

fail:
        unit_file_changes_free(changes, n_changes);
        return r;
}

static int method_enable_unit_files_generic(
                sd_bus *bus,
                sd_bus_message *message,
                Manager *m, const
                char *verb,
                int (*call)(UnitFileScope scope, bool runtime, const char *root_dir, char *files[], bool force, UnitFileChange **changes, unsigned *n_changes),
                bool carries_install_info,
                sd_bus_error *error) {

        _cleanup_strv_free_ char **l = NULL;
        UnitFileChange *changes = NULL;
        unsigned n_changes = 0;
        UnitFileScope scope;
        int runtime, force, r;

        assert(bus);
        assert(message);
        assert(m);

        r = selinux_access_check(bus, message, verb, error);
        if (r < 0)
                return r;

        r = sd_bus_message_read_strv(message, &l);
        if (r < 0)
                return r;

        r = sd_bus_message_read(message, "bb", &runtime, &force);
        if (r < 0)
                return r;

        scope = m->running_as == SYSTEMD_SYSTEM ? UNIT_FILE_SYSTEM : UNIT_FILE_USER;

        r = call(scope, runtime, NULL, l, force, &changes, &n_changes);
        if (r < 0)
                return r;

        return reply_unit_file_changes_and_free(m, bus, message, carries_install_info ? r : -1, changes, n_changes);
}

static int method_enable_unit_files(sd_bus *bus, sd_bus_message *message, void *userdata, sd_bus_error *error) {
        return method_enable_unit_files_generic(bus, message, userdata, "enable", unit_file_enable, true, error);
}

static int method_reenable_unit_files(sd_bus *bus, sd_bus_message *message, void *userdata, sd_bus_error *error) {
        return method_enable_unit_files_generic(bus, message, userdata, "enable", unit_file_reenable, true, error);
}

static int method_link_unit_files(sd_bus *bus, sd_bus_message *message, void *userdata, sd_bus_error *error) {
        return method_enable_unit_files_generic(bus, message, userdata, "enable", unit_file_link, false, error);
}

static int method_preset_unit_files(sd_bus *bus, sd_bus_message *message, void *userdata, sd_bus_error *error) {
        return method_enable_unit_files_generic(bus, message, userdata, "enable", unit_file_preset, true, error);
}

static int method_mask_unit_files(sd_bus *bus, sd_bus_message *message, void *userdata, sd_bus_error *error) {
        return method_enable_unit_files_generic(bus, message, userdata, "disable", unit_file_mask, false, error);
}

static int method_disable_unit_files_generic(
                sd_bus *bus,
                sd_bus_message *message,
                Manager *m, const
                char *verb,
                int (*call)(UnitFileScope scope, bool runtime, const char *root_dir, char *files[], UnitFileChange **changes, unsigned *n_changes),
                sd_bus_error *error) {

        _cleanup_strv_free_ char **l = NULL;
        UnitFileChange *changes = NULL;
        unsigned n_changes = 0;
        UnitFileScope scope;
        int r, runtime;

        assert(bus);
        assert(message);
        assert(m);

        r = selinux_access_check(bus, message, verb, error);
        if (r < 0)
                return r;

        r = sd_bus_message_read_strv(message, &l);
        if (r < 0)
                return r;

        r = sd_bus_message_read(message, "b", &runtime);
        if (r < 0)
                return r;

        scope = m->running_as == SYSTEMD_SYSTEM ? UNIT_FILE_SYSTEM : UNIT_FILE_USER;

        r = call(scope, runtime, NULL, l, &changes, &n_changes);
        if (r < 0)
                return r;

        return reply_unit_file_changes_and_free(m, bus, message, -1, changes, n_changes);
}

static int method_disable_unit_files(sd_bus *bus, sd_bus_message *message, void *userdata, sd_bus_error *error) {
        return method_disable_unit_files_generic(bus, message, userdata, "disable", unit_file_disable, error);
}

static int method_unmask_unit_files(sd_bus *bus, sd_bus_message *message, void *userdata, sd_bus_error *error) {
        return method_disable_unit_files_generic(bus, message, userdata, "enable", unit_file_unmask, error);
}

static int method_set_default_target(sd_bus *bus, sd_bus_message *message, void *userdata, sd_bus_error *error) {
        UnitFileChange *changes = NULL;
        unsigned n_changes = 0;
        Manager *m = userdata;
        UnitFileScope scope;
        const char *name;
        int force, r;

        assert(bus);
        assert(message);
        assert(m);

        r = selinux_access_check(bus, message, "enable", error);
        if (r < 0)
                return r;

        r = sd_bus_message_read(message, "sb", &name, &force);
        if (r < 0)
                return r;

        scope = m->running_as == SYSTEMD_SYSTEM ? UNIT_FILE_SYSTEM : UNIT_FILE_USER;

        r = unit_file_set_default(scope, NULL, name, force, &changes, &n_changes);
        if (r < 0)
                return r;

        return reply_unit_file_changes_and_free(m, bus, message, -1, changes, n_changes);
}

const sd_bus_vtable bus_manager_vtable[] = {
        SD_BUS_VTABLE_START(0),

        SD_BUS_PROPERTY("Version", "s", property_get_version, 0, 0),
        SD_BUS_PROPERTY("Features", "s", property_get_features, 0, 0),
        SD_BUS_PROPERTY("Virtualization", "s", property_get_virtualization, 0, 0),
        SD_BUS_PROPERTY("Tainted", "s", property_get_tainted, 0, 0),
        BUS_PROPERTY_DUAL_TIMESTAMP("FirmwareTimestamp", offsetof(Manager, firmware_timestamp), 0),
        BUS_PROPERTY_DUAL_TIMESTAMP("LoaderTimestamp", offsetof(Manager, loader_timestamp), 0),
        BUS_PROPERTY_DUAL_TIMESTAMP("KernelTimestamp", offsetof(Manager, firmware_timestamp), 0),
        BUS_PROPERTY_DUAL_TIMESTAMP("InitRDTimestamp", offsetof(Manager, initrd_timestamp), 0),
        BUS_PROPERTY_DUAL_TIMESTAMP("UserspaceTimestamp", offsetof(Manager, userspace_timestamp), 0),
        BUS_PROPERTY_DUAL_TIMESTAMP("FinishTimestamp", offsetof(Manager, finish_timestamp), 0),
        BUS_PROPERTY_DUAL_TIMESTAMP("SecurityStartTimestamp", offsetof(Manager, security_start_timestamp), 0),
        BUS_PROPERTY_DUAL_TIMESTAMP("SecurityFinishTimestamp", offsetof(Manager, security_finish_timestamp), 0),
        BUS_PROPERTY_DUAL_TIMESTAMP("GeneratorsStartTimestamp", offsetof(Manager, generators_start_timestamp), 0),
        BUS_PROPERTY_DUAL_TIMESTAMP("GeneratorsFinishTimestamp", offsetof(Manager, generators_finish_timestamp), 0),
        BUS_PROPERTY_DUAL_TIMESTAMP("UnitsLoadStartTimestamp", offsetof(Manager, units_load_start_timestamp), 0),
        BUS_PROPERTY_DUAL_TIMESTAMP("UnitsLoadFinishTimestamp", offsetof(Manager, units_load_finish_timestamp), 0),
        SD_BUS_WRITABLE_PROPERTY("LogLevel", "s", property_get_log_level, property_set_log_level, 0, 0),
        SD_BUS_WRITABLE_PROPERTY("LogTarget", "s", property_get_log_target, property_set_log_target, 0, 0),
        SD_BUS_PROPERTY("NNames", "u", property_get_n_names, 0, 0),
        SD_BUS_PROPERTY("NJobs", "u", property_get_n_jobs, 0, 0),
        SD_BUS_PROPERTY("NInstalledJobs", "u", bus_property_get_unsigned, offsetof(Manager, n_installed_jobs), 0),
        SD_BUS_PROPERTY("NFailedJobs", "u", bus_property_get_unsigned, offsetof(Manager, n_failed_jobs), 0),
        SD_BUS_PROPERTY("Progress", "d", property_get_progress, 0, 0),
        SD_BUS_PROPERTY("Environment", "as", NULL, offsetof(Manager, environment), 0),
        SD_BUS_PROPERTY("ConfirmSpawn", "b", bus_property_get_bool, offsetof(Manager, confirm_spawn), 0),
        SD_BUS_PROPERTY("ShowStatus", "b", bus_property_get_bool, offsetof(Manager, show_status), 0),
        SD_BUS_PROPERTY("UnitPath", "as", NULL, offsetof(Manager, lookup_paths.unit_path), 0),
        SD_BUS_PROPERTY("DefaultStandardOutput", "s", bus_property_get_exec_output, offsetof(Manager, default_std_output), 0),
        SD_BUS_PROPERTY("DefaultStandardError", "s", bus_property_get_exec_output, offsetof(Manager, default_std_output), 0),
        SD_BUS_WRITABLE_PROPERTY("RuntimeWatchdogUSec", "t", bus_property_get_usec, property_set_runtime_watchdog, offsetof(Manager, runtime_watchdog), 0),
        SD_BUS_WRITABLE_PROPERTY("ShutdownWatchdogUSec", "t", bus_property_get_usec, bus_property_set_usec, offsetof(Manager, shutdown_watchdog), 0),

        SD_BUS_METHOD("GetUnit", "s", "o", method_get_unit, 0),
        SD_BUS_METHOD("GetUnitByPID", "u", "o", method_get_unit_by_pid, 0),
        SD_BUS_METHOD("LoadUnit", "s", "o", method_load_unit, 0),
        SD_BUS_METHOD("StartUnit", "ss", "o", method_start_unit, 0),
        SD_BUS_METHOD("StartUnitReplace", "sss", "o", method_start_unit_replace, 0),
        SD_BUS_METHOD("StopUnit", "ss", "o", method_stop_unit, 0),
        SD_BUS_METHOD("ReloadUnit", "ss", "o", method_reload_unit, 0),
        SD_BUS_METHOD("RestartUnit", "ss", "o", method_restart_unit, 0),
        SD_BUS_METHOD("TryRestartUnit", "ss", "o", method_try_restart_unit, 0),
        SD_BUS_METHOD("ReloadOrRestartUnit", "ss", "o", method_reload_or_restart_unit, 0),
        SD_BUS_METHOD("ReloadOrTryRestartUnit", "ss", "o", method_reload_or_try_restart_unit, 0),
        SD_BUS_METHOD("KillUnit", "ssi", NULL, method_kill_unit, 0),
        SD_BUS_METHOD("ResetFailedUnit", "s", NULL, method_reset_failed_unit, 0),
        SD_BUS_METHOD("SetUnitProperties", "sb", "a(sv)", method_set_unit_properties, 0),
        SD_BUS_METHOD("StartTransientUnit", "ssa(sv)a(sa(sv))", "o", method_start_transient_unit, 0),
        SD_BUS_METHOD("GetJob", "u", "o", method_get_job, 0),
        SD_BUS_METHOD("CancelJob", "u", NULL, method_cancel_job, 0),
        SD_BUS_METHOD("ClearJobs", NULL, NULL, method_clear_jobs, 0),
        SD_BUS_METHOD("ResetFailed", NULL, NULL, method_reset_failed, 0),
        SD_BUS_METHOD("ListUnits", NULL, "a(ssssssouso)", method_list_units, 0),
        SD_BUS_METHOD("ListJobs", NULL, "a(usssoo)", method_list_jobs, 0),
        SD_BUS_METHOD("Subscribe", NULL, NULL, method_subscribe, 0),
        SD_BUS_METHOD("Unsubscribe", NULL, NULL, method_unsubscribe, 0),
        SD_BUS_METHOD("Dump", NULL, "s", method_dump, 0),
        SD_BUS_METHOD("CreateSnapshot", "sb", "o", method_create_snapshot, 0),
        SD_BUS_METHOD("RemoveSnapshot", "s", NULL, method_remove_snapshot, 0),
        SD_BUS_METHOD("Reload", NULL, NULL, method_reload, 0),
        SD_BUS_METHOD("Reexecute", NULL, NULL, method_reexecute, 0),
        SD_BUS_METHOD("Exit", NULL, NULL, method_exit, 0),
        SD_BUS_METHOD("Reboot", NULL, NULL, method_reboot, 0),
        SD_BUS_METHOD("PowerOff", NULL, NULL, method_poweroff, 0),
        SD_BUS_METHOD("Halt", NULL, NULL, method_halt, 0),
        SD_BUS_METHOD("KExec", NULL, NULL, method_kexec, 0),
        SD_BUS_METHOD("SwitchRoot", "ss", NULL, method_switch_root, 0),
        SD_BUS_METHOD("SetEnvironment", "as", NULL, method_set_environment, 0),
        SD_BUS_METHOD("UnsetEnvironment", "as", NULL, method_unset_environment, 0),
        SD_BUS_METHOD("UnsetAndSetEnvironment", "asas", NULL, method_unset_and_set_environment, 0),
        SD_BUS_METHOD("ListUnitFiles", NULL, "a(ss)", method_list_unit_files, 0),
        SD_BUS_METHOD("GetUnitFileState", "s", "s", method_get_unit_file_state, 0),
        SD_BUS_METHOD("EnableUnitFiles", "asbb", "ba(sss)", method_enable_unit_files, 0),
        SD_BUS_METHOD("DisableUnitFiles", "asb", "a(sss)", method_disable_unit_files, 0),
        SD_BUS_METHOD("ReenableUnitFiles", "asbb", "ba(sss)", method_reenable_unit_files, 0),
        SD_BUS_METHOD("LinkUnitFiles", "asbb", "a(sss)", method_link_unit_files, 0),
        SD_BUS_METHOD("PresetUnitFiles", "asbb", "ba(sss)", method_preset_unit_files, 0),
        SD_BUS_METHOD("MaskUnitFiles", "asbb", "a(sss)", method_mask_unit_files, 0),
        SD_BUS_METHOD("UnmaskUnitFiles", "asb", "a(sss)", method_unmask_unit_files, 0),
        SD_BUS_METHOD("SetDefaultTarget", "sb", "a(sss)", method_set_default_target, 0),
        SD_BUS_METHOD("GetDefaultTarget", NULL, "s", method_get_default_target, 0),

        SD_BUS_SIGNAL("UnitNew", "so", 0),
        SD_BUS_SIGNAL("UnitRemoved", "so", 0),
        SD_BUS_SIGNAL("JobNew", "uos", 0),
        SD_BUS_SIGNAL("JobRemoved", "uoss", 0),
        SD_BUS_SIGNAL("StartupFinished", "tttttt", 0),
        SD_BUS_SIGNAL("UnitFilesChanged", NULL, 0),
        SD_BUS_SIGNAL("Reloading", "b", 0),

        SD_BUS_VTABLE_END
};

int bus_manager_foreach_client(Manager *m, int (*send_message)(sd_bus *bus, const char *destination, void *userdata), void *userdata) {
        Iterator i;
        sd_bus *b;
        unsigned n;
        int r;

        n = set_size(m->subscribed);
        if (n <= 0)
                return 0;
        if (n == 1) {
                BusTrackedClient *d;

                assert_se(d = set_first(m->subscribed));
                return send_message(d->bus, isempty(d->name) ? NULL : d->name, userdata);
        }

        /* Send to everybody */
        SET_FOREACH(b, m->private_buses, i) {
                r = send_message(b, NULL, userdata);
                if (r < 0)
                        return r;
        }

        if (m->api_bus)
                return send_message(m->api_bus, NULL, userdata);

        return 0;
}

static int send_finished(sd_bus *bus, const char *destination, void *userdata) {
        _cleanup_bus_message_unref_ sd_bus_message *message = NULL;
        usec_t *times = userdata;
        int r;

        assert(bus);
        assert(times);

        r = sd_bus_message_new_signal(bus, "/org/freedesktop/systemd1", "org.freedesktop.systemd1.Manager", "StartupFinished", &message);
        if (r < 0)
                return r;

        r = sd_bus_message_append(message, "tttttt", times[0], times[1], times[2], times[3], times[4], times[5]);
        if (r < 0)
                return r;

        return sd_bus_send_to(bus, message, destination, NULL);
}

int bus_manager_send_finished(
                Manager *m,
                usec_t firmware_usec,
                usec_t loader_usec,
                usec_t kernel_usec,
                usec_t initrd_usec,
                usec_t userspace_usec,
                usec_t total_usec) {

        assert(m);

        return bus_manager_foreach_client(m, send_finished,
                        (usec_t[6]) { firmware_usec, loader_usec, kernel_usec, initrd_usec, userspace_usec, total_usec });
}

static int send_reloading(sd_bus *bus, const char *destination, void *userdata) {
        _cleanup_bus_message_unref_ sd_bus_message *message = NULL;
        int r;

        assert(bus);

        r = sd_bus_message_new_signal(bus, "/org/freedesktop/systemd1", "org.freedesktop.systemd1.Manager", "Reloading", &message);
        if (r < 0)
                return r;

        r = sd_bus_message_append(message, "b", PTR_TO_INT(userdata));
        if (r < 0)
                return r;

        return sd_bus_send_to(bus, message, destination, NULL);
}

int bus_manager_send_reloading(Manager *m, bool active) {
        assert(m);

        return bus_manager_foreach_client(m, send_reloading, INT_TO_PTR(active));
}
