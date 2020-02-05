#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/acpi.h>
#include <linux/suspend.h> /* for pm notifier */
#include <linux/proc_fs.h>
#include <linux/uaccess.h>
#include <linux/input.h>
#include <linux/seq_file.h>

#define CLEVO_WMI_VERSION "1.0"

MODULE_AUTHOR("simopil");
MODULE_DESCRIPTION("Clevo W35_37ET WMI driver based of Peter Wu's clevo-wmi");
MODULE_LICENSE("GPL");
MODULE_VERSION(CLEVO_WMI_VERSION);

/* this ID seems to be used in example documents, not just Clevo ACPI */
/* GUID for method BB */
#define CLEVO_WMI_WMBB_GUID	"ABBC0F6D-8EA1-11D1-00A0-C90629100000"
/* GUID for event D0 */
#define CLEVO_WMI_EVD0_GUID	"ABBC0F6B-8EA1-11D1-00A0-C90629100000"

#define CLEVO_WMI_FUNC_GET_EVENT		0x01
#define CLEVO_WMI_FUNC_ENABLE_NOTIFICATIONS	0x46
#define CLEVO_WMI_FUNC_SET_LED			0x56

#define VGA_LED_STATUS_OFFSET 249
#define FAN_SPEED_MAGIC_NUMBER 1966080

#define CLEVO_WMI_KEY_VGA KEY_PROG1
#define CLEVO_WMI_KEY_ESC KEY_PROG2

static int init_color = 1;
MODULE_PARM_DESC(init_color, "VGA led color on module load (1=green | 0=yellow)");
module_param(init_color, int, 0600);

struct clevo_wmi {
	struct input_dev *input_dev;
	/* used for enabling WMI again on resume from suspend */
	struct notifier_block pm_notifier;
};

static struct clevo_wmi clevo_priv;

static int call_wmbb(int func, u8 args, u32 *result) {
	struct acpi_buffer output = { ACPI_ALLOCATE_BUFFER, NULL };
	struct acpi_buffer input = { sizeof(args), &args };
	union acpi_object *obj;
	acpi_status status;
	int ret = 0;

	status = wmi_evaluate_method(CLEVO_WMI_WMBB_GUID,
		0 /* ignored by WMBB method */, func, &input, &output);
	if (ACPI_FAILURE(status)) {
		pr_warn("Failed to execute function %#02x: %s\n", func,
			acpi_format_exception(status));
		return -1;
	}
	obj = (union acpi_object *)output.pointer;
	if (obj->type == ACPI_TYPE_INTEGER) {
		u32 val = obj->integer.value;
		if (val == 0xFFFFFFFF) {
			pr_warn("Invalid function: %#02x\n", func);
			ret = -1;
		}
		if (result)
			*result = val;
	} else {
		pr_warn("Unexpected output type: %i\n", obj->type);
		ret = -1;
	}
	kfree(output.pointer);
	return ret;
}

int fanspeed_read(struct seq_file *m, void *v)
{
    //ec offsets for fan speed are D0 and D1 (208, 209)
    u8 fan_data[2];
    char rawdata[4];
    long int rpms = 0;
    (void)v; /* Unused */

    ec_read(208, &fan_data[0]);
    ec_read(209, &fan_data[1]);
    sprintf(rawdata, "%x%x", fan_data[0], fan_data[1]);
    if(kstrtol(rawdata, 16, &rpms)) return -EFAULT;
    if(rpms == 0)
        seq_printf(m, "0\n");
    else
        seq_printf(m, "%ld\n", FAN_SPEED_MAGIC_NUMBER/rpms);
    return 0;
}

int fanspeed_proc_open(struct inode *inode, struct file *filp)
{
    return single_open(filp, &fanspeed_read, NULL);
}

int ledread(struct seq_file *m, void *v)
{
    //ec offset for get led status = 249
    u8 led_status;
    (void)v; /* Unused */
    ec_read(VGA_LED_STATUS_OFFSET, &led_status);
    if( led_status == 0 ){
        seq_printf(m, "GREEN\n");
    } else {
        seq_printf(m, "YELLOW\n");
    }
    return 0;
}

int vgaled_proc_open(struct inode *inode, struct file *filp)
{
    return single_open(filp, &ledread, NULL);
}

static ssize_t ledwrite(struct file *file, const char __user *ubuf, size_t count, loff_t *ppos)
{
	char input[3];
    if (count >= sizeof(input)) count = sizeof(input) - 1;
	if(copy_from_user(input,ubuf,count)) return -EFAULT;
    if(strncmp(input, "G", 1) == 0 || strncmp(input, "g", 1) == 0) {
        call_wmbb(CLEVO_WMI_FUNC_SET_LED, 1, NULL);
        pr_info("Setting VGA_LED to green");
    }
    else if(strncmp(input, "Y", 1) == 0 || strncmp(input, "y", 1) == 0) {
        call_wmbb(CLEVO_WMI_FUNC_SET_LED, 0, NULL);
        pr_info("Setting VGA_LED to yellow");
    }
	return count;
}

static const struct file_operations vgaled_fops = {
    .owner = THIS_MODULE,
    .write = ledwrite,
    .open  = &vgaled_proc_open,
    .read  = &seq_read,
};

static const struct file_operations fanrpm_fops = {
    .owner = THIS_MODULE,
    .open  = &fanspeed_proc_open,
    .read  = &seq_read,
};

/* send and release a key */
static void clevo_wmi_send_key(unsigned int code) {
	input_report_key(clevo_priv.input_dev, code, 1);
	input_sync(clevo_priv.input_dev);
	input_report_key(clevo_priv.input_dev, code, 0);
	input_sync(clevo_priv.input_dev);
}

static void clevo_wmi_notify(u32 value, void *context) {
	u32 event = 0;
	if (call_wmbb(CLEVO_WMI_FUNC_GET_EVENT,
		0 /* args are ignored */, &event)) {
		pr_warn("Could not get WMI event number.\n");
		return;
	}
	pr_debug("Event number: %#02x\n", event);
    pr_info("key %x pressed\n", event);
	switch (event) {
	case 0xA3:
        {
            clevo_wmi_send_key(CLEVO_WMI_KEY_VGA);
            break;
        }
    case 0x9A:
		{
            clevo_wmi_send_key(CLEVO_WMI_KEY_ESC);
		    break;
	    }
    }
}

/**
 * Make the firmware generate WMI events.
 */
static int clevo_wmi_enable(void) {
	u32 result = 0;
	if (call_wmbb(CLEVO_WMI_FUNC_ENABLE_NOTIFICATIONS,
		0 /* args are ignored */, &result)) {
		pr_err("Unable to enable WMI notifications\n");
		return -ENODEV;
	}
	pr_debug("Enabling WMI notifications yields: %#04x\n", result);
	return 0;
}

static int clevo_wmi_pm_handler(struct notifier_block *nbp,
	unsigned long event_type, void *p) {
	switch (event_type) {
	case PM_POST_HIBERNATION:
	case PM_POST_SUSPEND:
	case PM_POST_RESTORE:
		clevo_wmi_enable();
		break;
	}
	return 0;
}


static int __init clevo_wmi_input_setup(void) {
	int error;
	struct input_dev *input_dev = input_allocate_device();
	if (!input_dev) {
		pr_err("Not enough memory for input device.\n");
		return -ENOMEM;
	}

	input_dev->name = "Clevo WMI hotkeys";
	set_bit(EV_KEY, input_dev->evbit);
	set_bit(CLEVO_WMI_KEY_VGA, input_dev->keybit);
    set_bit(CLEVO_WMI_KEY_ESC, input_dev->keybit);

	error = input_register_device(input_dev);
	if (error) {
		pr_err("Failed to register input device.\n");
		input_free_device(input_dev);
		return error;
	}
	clevo_priv.input_dev = input_dev;
	return 0;
}

static int __init clevo_wmi_init(void) {
    int ret, error;
    proc_create("fan_rpm", 0444,acpi_root_dir,&fanrpm_fops);
    proc_create("vga_led", 0664,acpi_root_dir,&vgaled_fops);
    if(init_color==1 || init_color==0) {
        call_wmbb(CLEVO_WMI_FUNC_SET_LED, init_color, NULL);
    } else pr_info("Invalid init_color parameter! Ignoring...");

	if (!wmi_has_guid(CLEVO_WMI_WMBB_GUID)) {
		pr_err("Clevo WMI GUID not found\n");
		return -ENODEV;
	}

	error = clevo_wmi_input_setup();
	if (error)
		return error;

	if ((error = clevo_wmi_enable()))
		goto err_unregister_input_dev;

	ret = wmi_install_notify_handler(CLEVO_WMI_EVD0_GUID, clevo_wmi_notify,
		NULL);
	if (ACPI_FAILURE(ret)) {
		pr_err("Could not register WMI notifier for Clevo: %s.\n",
			acpi_format_exception(ret));
		error = -EINVAL;
		goto err_unregister_input_dev;
	}

	clevo_priv.pm_notifier.notifier_call = &clevo_wmi_pm_handler;
	error = register_pm_notifier(&clevo_priv.pm_notifier);
	if (error)
		goto err_remove_wmi_notifier;

	pr_info("Clevo WMI driver loaded.\n");
	return 0;

err_remove_wmi_notifier:
	wmi_remove_notify_handler(CLEVO_WMI_EVD0_GUID);
err_unregister_input_dev:
	input_unregister_device(clevo_priv.input_dev);
	return error;
}

static void __exit clevo_wmi_exit(void)
{
	remove_proc_entry("vga_led", acpi_root_dir);
    remove_proc_entry("fan_rpm", acpi_root_dir);
    unregister_pm_notifier(&clevo_priv.pm_notifier);
	wmi_remove_notify_handler(CLEVO_WMI_EVD0_GUID);
	input_unregister_device(clevo_priv.input_dev);
	pr_info("Clevo WMI driver unloaded.\n");
}

module_init(clevo_wmi_init);
module_exit(clevo_wmi_exit);
