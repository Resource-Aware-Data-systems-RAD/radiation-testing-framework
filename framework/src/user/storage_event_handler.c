#include <stdio.h>
#include <stdlib.h>
#include <bpf/libbpf.h>
#include "../../include/event_types.h"
#include "../../include/event_handler.h"
#include "../../include/logger.h"
#include "storage_monitor.skel.h"

static struct storage_monitor_bpf *storage_skel;
static struct ring_buffer *rb_storage;
static storage_handler_config_t *storage_config;

int handle_storage_event(void *ctx, void *data, uint64_t data_sz)
{
	storage_event_t *event = data;
	char *log_string = (char *)malloc(100);
	sprintf(log_string,
		"[STORAGE ERROR] Device: %u | Sector: %lu | Error Code: %u | Time: %lu ns\n",
		event->device, event->sector, event->error_code, event->timestamp);
	logger_log(log_string);
	free(log_string);
	return 0;
}

int setup_storage_monitor()
{
	struct bpf_object_open_opts opts = {
		.sz = sizeof(struct bpf_object_open_opts),
		.object_name = "storage_monitor",
	};

	storage_skel = storage_monitor_bpf__open_opts(&opts);
	if (!storage_skel) {
		fprintf(stderr, "Failed to open Storage monitor BPF program\n");
		return -1;
	}

	if (storage_monitor_bpf__load(storage_skel) || storage_monitor_bpf__attach(storage_skel)) {
		fprintf(stderr, "Failed to load or attach Storage monitor BPF program\n");
		return -1;
	}

	rb_storage = ring_buffer__new(bpf_map__fd(storage_skel->maps.storage_events),
				      handle_storage_event, NULL, NULL);
	if (!rb_storage) {
		fprintf(stderr, "Failed to create ring buffer for Storage events\n");
		return -1;
	}

	create_directory_if_not_exists("/sys/fs/bpf/radiation_testing_framework");

	// pin all of the maps
	create_directory_if_not_exists("/sys/fs/bpf/radiation_testing_framework/map");
	bpf_map__pin(storage_skel->maps.storage_events,
		     "/sys/fs/bpf/radiation_testing_framework/map/storage_events");

	// pin all of the programs
	create_directory_if_not_exists("/sys/fs/bpf/radiation_testing_framework/prog");
	bpf_program__pin(storage_skel->progs.trace_block_rq_complete,
			 "/sys/fs/bpf/radiation_testing_framework/prog/trace_block_rq_complete");

	return 0;
}

int poll_storage_events()
{
	return ring_buffer__poll(rb_storage, 100);
}

void cleanup_storage_monitor()
{
	// unpin the maps
	bpf_map__unpin(storage_skel->maps.storage_events,
		       "/sys/fs/bpf/radiation_testing_framework/map/storage_events");
	// unping the programs
	bpf_program__unpin(storage_skel->progs.trace_block_rq_complete,
			   "/sys/fs/bpf/radiation_testing_framework/prog/trace_block_rq_complete");
	ring_buffer__free(rb_storage);
	storage_monitor_bpf__destroy(storage_skel);
	if (storage_config->param2) {
		free(storage_config->param2);
	}
	free(storage_config);
}

int add_storage_config_key(const char *name, const char *value)
{
	if (!storage_config) {
		storage_config =
			(storage_handler_config_t *)malloc(sizeof(storage_handler_config_t));
		if (!storage_config) {
			fprintf(stderr, "Failed to allocate memory for Storage handler config\n");
			return 0; // error
		}
	}

	if (strcmp(name, "param1") == 0) {
		storage_config->param1 = atoi(value);
	} else if (strcmp(name, "param2") == 0) {
		storage_config->param2 = malloc(strlen(value) + 1);
		if (!storage_config->param2) {
			fprintf(stderr, "Failed to allocate memory for Storage handler config\n");
			return 0; // error
		}
		strcpy(storage_config->param2, value);
	} else {
		fprintf(stderr, "Unknown configuration key: %s\n", name);
		return 0; // not found
	}

	return 1; // success
}

handler_t storage_handler = {
	.name = "storage_handler",
	.setup = setup_storage_monitor,
	.poll = poll_storage_events,
	.cleanup = cleanup_storage_monitor,
	.add_config_key = add_storage_config_key,
};
