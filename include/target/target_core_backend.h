#ifndef TARGET_CORE_BACKEND_H
#define TARGET_CORE_BACKEND_H

#define TRANSPORT_PLUGIN_PHBA_PDEV		1
#define TRANSPORT_PLUGIN_VHBA_PDEV		2
#define TRANSPORT_PLUGIN_VHBA_VDEV		3

struct se_subsystem_api {
	struct list_head sub_api_list;

	char name[16];
	struct module *owner;

	u8 transport_type;

	unsigned int fua_write_emulated : 1;
	unsigned int write_cache_emulated : 1;

	int (*attach_hba)(struct se_hba *, u32);
	void (*detach_hba)(struct se_hba *);
	int (*pmode_enable_hba)(struct se_hba *, unsigned long);
	void *(*allocate_virtdevice)(struct se_hba *, const char *);
	struct se_device *(*create_virtdevice)(struct se_hba *,
				struct se_subsystem_dev *, void *);
	void (*free_device)(void *);
	int (*transport_complete)(struct se_task *task);
	struct se_task *(*alloc_task)(unsigned char *cdb);
	int (*alloc_cmd_mem)(struct se_cmd *);
	void (*free_cmd_mem)(struct se_cmd *);
	int (*do_pr_offload)(struct se_task *);
	int (*do_persistent_reserve)(struct se_task *task, u8 sa, u8 scope, u8 type);
	int (*do_task)(struct se_task *);
	int (*do_discard)(struct se_device *, sector_t, u32);
	int (*do_write_same)(struct se_task *task, sector_t lba, u32 range);
	int (*do_compare_and_write)(struct se_task *task, u32 range);
	int (*do_lun_reset)(struct se_tmr_req *, struct completion *);
	void (*do_sync_cache)(struct se_task *);
	void (*free_task)(struct se_task *);
	ssize_t (*check_configfs_dev_params)(struct se_hba *,
			struct se_subsystem_dev *);
	ssize_t (*set_configfs_dev_params)(struct se_hba *,
			struct se_subsystem_dev *, const char *, ssize_t);
	ssize_t (*show_configfs_dev_params)(struct se_hba *,
			struct se_subsystem_dev *, char *);
	u32 (*get_device_rev)(struct se_device *);
	u32 (*get_device_type)(struct se_device *);
	sector_t (*get_blocks)(struct se_device *);
	unsigned char *(*get_sense_buffer)(struct se_task *);
	const char * (*get_volume_name)(struct se_device *);
};

int	transport_subsystem_register(struct se_subsystem_api *);
void	transport_subsystem_release(struct se_subsystem_api *);

struct se_device *transport_add_device_to_core_hba(struct se_hba *,
		struct se_subsystem_api *, struct se_subsystem_dev *, u32,
		void *, struct se_dev_limits *, const char *, const char *);

void	transport_complete_sync_cache(struct se_cmd *, int);
void	transport_complete_task(struct se_task *, int);

void	target_get_task_cdb(struct se_task *, unsigned char *);

void	transport_set_vpd_proto_id(struct t10_vpd *, unsigned char *);
int	transport_set_vpd_assoc(struct t10_vpd *, unsigned char *);
int	transport_set_vpd_ident_type(struct t10_vpd *, unsigned char *);
int	transport_set_vpd_ident(struct t10_vpd *, unsigned char *);

/* core helpers also used by command snooping in pscsi */
void	*transport_kmap_data_sg(struct se_cmd *);
void	transport_kunmap_data_sg(struct se_cmd *);

void	array_free(void *array, int n);

#endif /* TARGET_CORE_BACKEND_H */
