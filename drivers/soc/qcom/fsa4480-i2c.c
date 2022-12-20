// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2018-2020, The Linux Foundation. All rights reserved.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/power_supply.h>
#include <linux/regmap.h>
#include <linux/i2c.h>
#include <linux/mutex.h>
#include <linux/soc/qcom/fsa4480-i2c.h>


#define FSA4480_I2C_NAME	"fsa4480-driver"

#define FSA4480_DEVICE_ID       0x00
#define FSA4480_SWITCH_SETTINGS 0x04
#define FSA4480_SWITCH_CONTROL  0x05
#define FSA4480_SWITCH_STATUS1  0x07
#define FSA4480_SLOW_L          0x08
#define FSA4480_SLOW_R          0x09
#define FSA4480_SLOW_MIC        0x0A
#define FSA4480_SLOW_SENSE      0x0B
#define FSA4480_SLOW_GND        0x0C
#define FSA4480_DELAY_L_R       0x0D
#define FSA4480_DELAY_L_MIC     0x0E
#define FSA4480_DELAY_L_SENSE   0x0F
#define FSA4480_DELAY_L_AGND    0x10
#define FSA4480_RESET           0x1E

struct fsa4480_priv {
	struct regmap *regmap;
	struct device *dev;
	struct power_supply *usb_psy;
	struct notifier_block psy_nb;
	atomic_t usbc_mode;
	struct work_struct usbc_analog_work;
	struct blocking_notifier_head fsa4480_notifier;
	struct mutex notification_lock;
};

#ifdef CONFIG_QCOM_FSA4480_LPD
struct fsa4480_priv *g_fsa4480_priv = NULL;
#endif

struct fsa4480_reg_val {
	u16 reg;
	u8 val;
};

static const struct regmap_config fsa4480_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
	.max_register = FSA4480_RESET,
};

static const struct fsa4480_reg_val fsa_reg_i2c_defaults[] = {
	{FSA4480_SLOW_L, 0x00},
	{FSA4480_SLOW_R, 0x00},
	{FSA4480_SLOW_MIC, 0x00},
	{FSA4480_SLOW_SENSE, 0x00},
	{FSA4480_SLOW_GND, 0x00},
	{FSA4480_DELAY_L_R, 0x00},
	{FSA4480_DELAY_L_MIC, 0x00},
	{FSA4480_DELAY_L_SENSE, 0x00},
	{FSA4480_DELAY_L_AGND, 0x09},
	{FSA4480_SWITCH_SETTINGS, 0x98},
};

static void fsa4480_usbc_update_settings(struct fsa4480_priv *fsa_priv,
		u32 switch_control, u32 switch_enable)
{
	if (!fsa_priv->regmap) {
		dev_err(fsa_priv->dev, "%s: regmap invalid\n", __func__);
		return;
	}

	regmap_write(fsa_priv->regmap, FSA4480_SWITCH_SETTINGS, 0x80);
	regmap_write(fsa_priv->regmap, FSA4480_SWITCH_CONTROL, switch_control);
	/* FSA4480 chip hardware requirement */
	usleep_range(50, 55);
	regmap_write(fsa_priv->regmap, FSA4480_SWITCH_SETTINGS, switch_enable);
}

static int fsa4480_usbc_event_changed(struct notifier_block *nb,
				      unsigned long evt, void *ptr)
{
	int ret;
	union power_supply_propval mode;
	struct fsa4480_priv *fsa_priv =
			container_of(nb, struct fsa4480_priv, psy_nb);
	struct device *dev;

	if (!fsa_priv)
		return -EINVAL;

	dev = fsa_priv->dev;
	if (!dev)
		return -EINVAL;

	if ((struct power_supply *)ptr != fsa_priv->usb_psy ||
				evt != PSY_EVENT_PROP_CHANGED)
		return 0;

	ret = power_supply_get_property(fsa_priv->usb_psy,
			POWER_SUPPLY_PROP_TYPEC_MODE, &mode);
	if (ret) {
		dev_err(dev, "%s: Unable to read USB TYPEC_MODE: %d\n",
			__func__, ret);
		return ret;
	}

	dev_dbg(dev, "%s: USB change event received, supply mode %d, usbc mode %d, expected %d\n",
		__func__, mode.intval, fsa_priv->usbc_mode.counter,
		POWER_SUPPLY_TYPEC_SINK_AUDIO_ADAPTER);

	switch (mode.intval) {
	case POWER_SUPPLY_TYPEC_SINK_AUDIO_ADAPTER:
	case POWER_SUPPLY_TYPEC_NONE:
		if (atomic_read(&(fsa_priv->usbc_mode)) == mode.intval)
			break; /* filter notifications received before */
		atomic_set(&(fsa_priv->usbc_mode), mode.intval);

		dev_dbg(dev, "%s: queueing usbc_analog_work\n",
			__func__);
		pm_stay_awake(fsa_priv->dev);
		queue_work(system_freezable_wq, &fsa_priv->usbc_analog_work);
		break;
	default:
		break;
	}
	return ret;
}

static int fsa4480_usbc_analog_setup_switches(struct fsa4480_priv *fsa_priv)
{
	int rc = 0;
	union power_supply_propval mode;
	struct device *dev;

	if (!fsa_priv)
		return -EINVAL;
	dev = fsa_priv->dev;
	if (!dev)
		return -EINVAL;

	mutex_lock(&fsa_priv->notification_lock);
	/* get latest mode again within locked context */
	rc = power_supply_get_property(fsa_priv->usb_psy,
			POWER_SUPPLY_PROP_TYPEC_MODE, &mode);
	if (rc) {
		dev_err(dev, "%s: Unable to read USB TYPEC_MODE: %d\n",
			__func__, rc);
		goto done;
	}
	dev_dbg(dev, "%s: setting GPIOs active = %d\n",
		__func__, mode.intval != POWER_SUPPLY_TYPEC_NONE);

	switch (mode.intval) {
	/* add all modes FSA should notify for in here */
	case POWER_SUPPLY_TYPEC_SINK_AUDIO_ADAPTER:
		/* activate switches */
		fsa4480_usbc_update_settings(fsa_priv, 0x00, 0x9F);

		/* notify call chain on event */
		blocking_notifier_call_chain(&fsa_priv->fsa4480_notifier,
		mode.intval, NULL);
		break;
	case POWER_SUPPLY_TYPEC_NONE:
		/* notify call chain on event */
		blocking_notifier_call_chain(&fsa_priv->fsa4480_notifier,
				POWER_SUPPLY_TYPEC_NONE, NULL);

		/* deactivate switches */
		fsa4480_usbc_update_settings(fsa_priv, 0x18, 0x98);
		break;
	default:
		/* ignore other usb connection modes */
		break;
	}

done:
	mutex_unlock(&fsa_priv->notification_lock);
	return rc;
}

/*
 * fsa4480_reg_notifier - register notifier block with fsa driver
 *
 * @nb - notifier block of fsa4480
 * @node - phandle node to fsa4480 device
 *
 * Returns 0 on success, or error code
 */
int fsa4480_reg_notifier(struct notifier_block *nb,
			 struct device_node *node)
{
	int rc = 0;
	struct i2c_client *client = of_find_i2c_device_by_node(node);
	struct fsa4480_priv *fsa_priv;

	if (!client)
		return -EINVAL;

	fsa_priv = (struct fsa4480_priv *)i2c_get_clientdata(client);
	if (!fsa_priv)
		return -EINVAL;

	rc = blocking_notifier_chain_register
				(&fsa_priv->fsa4480_notifier, nb);
	if (rc)
		return rc;

	/*
	 * as part of the init sequence check if there is a connected
	 * USB C analog adapter
	 */
	dev_dbg(fsa_priv->dev, "%s: verify if USB adapter is already inserted\n",
		__func__);
	rc = fsa4480_usbc_analog_setup_switches(fsa_priv);

	return rc;
}
EXPORT_SYMBOL(fsa4480_reg_notifier);

/*
 * fsa4480_unreg_notifier - unregister notifier block with fsa driver
 *
 * @nb - notifier block of fsa4480
 * @node - phandle node to fsa4480 device
 *
 * Returns 0 on pass, or error code
 */
int fsa4480_unreg_notifier(struct notifier_block *nb,
			     struct device_node *node)
{
	int rc = 0;
	struct i2c_client *client = of_find_i2c_device_by_node(node);
	struct fsa4480_priv *fsa_priv;
	struct device *dev;
	union power_supply_propval mode;

	if (!client)
		return -EINVAL;

	fsa_priv = (struct fsa4480_priv *)i2c_get_clientdata(client);
	if (!fsa_priv)
		return -EINVAL;
	dev = fsa_priv->dev;
	if (!dev)
		return -EINVAL;

	mutex_lock(&fsa_priv->notification_lock);
	/* get latest mode within locked context */
	rc = power_supply_get_property(fsa_priv->usb_psy,
			POWER_SUPPLY_PROP_TYPEC_MODE, &mode);
	if (rc) {
		dev_dbg(dev, "%s: Unable to read USB TYPEC_MODE: %d\n",
			__func__, rc);
		goto done;
	}
	/* Do not reset switch settings for usb digital hs */
	if (mode.intval == POWER_SUPPLY_TYPEC_SINK_AUDIO_ADAPTER)
		fsa4480_usbc_update_settings(fsa_priv, 0x18, 0x98);
	rc = blocking_notifier_chain_unregister
					(&fsa_priv->fsa4480_notifier, nb);
done:
	mutex_unlock(&fsa_priv->notification_lock);
	return rc;
}
EXPORT_SYMBOL(fsa4480_unreg_notifier);

static int fsa4480_validate_display_port_settings(struct fsa4480_priv *fsa_priv)
{
	u32 switch_status = 0;

	regmap_read(fsa_priv->regmap, FSA4480_SWITCH_STATUS1, &switch_status);

	if ((switch_status != 0x23) && (switch_status != 0x1C)) {
		pr_err("AUX SBU1/2 switch status is invalid = %u\n",
				switch_status);
		return -EIO;
	}

	return 0;
}
/*
 * fsa4480_switch_event - configure FSA switch position based on event
 *
 * @node - phandle node to fsa4480 device
 * @event - fsa_function enum
 *
 * Returns int on whether the switch happened or not
 */
int fsa4480_switch_event(struct device_node *node,
			 enum fsa_function event)
{
	int switch_control = 0;
	struct i2c_client *client = of_find_i2c_device_by_node(node);
	struct fsa4480_priv *fsa_priv;

	if (!client)
		return -EINVAL;

	fsa_priv = (struct fsa4480_priv *)i2c_get_clientdata(client);
	if (!fsa_priv)
		return -EINVAL;
	if (!fsa_priv->regmap)
		return -EINVAL;

	switch (event) {
	case FSA_MIC_GND_SWAP:
		regmap_read(fsa_priv->regmap, FSA4480_SWITCH_CONTROL,
				&switch_control);
		if ((switch_control & 0x07) == 0x07)
			switch_control = 0x0;
		else
			switch_control = 0x7;
		fsa4480_usbc_update_settings(fsa_priv, switch_control, 0x9F);
		break;
	case FSA_USBC_ORIENTATION_CC1:
		fsa4480_usbc_update_settings(fsa_priv, 0x18, 0xF8);
		return fsa4480_validate_display_port_settings(fsa_priv);
	case FSA_USBC_ORIENTATION_CC2:
		fsa4480_usbc_update_settings(fsa_priv, 0x78, 0xF8);
		return fsa4480_validate_display_port_settings(fsa_priv);
	case FSA_USBC_DISPLAYPORT_DISCONNECTED:
		fsa4480_usbc_update_settings(fsa_priv, 0x18, 0x98);
		break;
	default:
		break;
	}

	return 0;
}
EXPORT_SYMBOL(fsa4480_switch_event);

static void fsa4480_usbc_analog_work_fn(struct work_struct *work)
{
	struct fsa4480_priv *fsa_priv =
		container_of(work, struct fsa4480_priv, usbc_analog_work);

	if (!fsa_priv) {
		pr_err("%s: fsa container invalid\n", __func__);
		return;
	}
	fsa4480_usbc_analog_setup_switches(fsa_priv);
	pm_relax(fsa_priv->dev);
}

static void fsa4480_update_reg_defaults(struct regmap *regmap)
{
	u8 i;

	for (i = 0; i < ARRAY_SIZE(fsa_reg_i2c_defaults); i++)
		regmap_write(regmap, fsa_reg_i2c_defaults[i].reg,
				   fsa_reg_i2c_defaults[i].val);
}

#ifdef CONFIG_QCOM_FSA4480_LPD
#define FSA4480_RES_ENABLE                            0x12
#define FSA4480_RES_ENABLE_BIT                          BIT(1)
#define FSA4480_RES_ENABLE_RES_RANGE                          BIT(5)
#define FSA4480_RES_DETECT_PIN                    0x13
#define FSA4480_RES_DETECT_PIN_SELECT      GENMASK(2, 0)
#define FSA4480_RES_VALUE                              0x14
#define FSA4480_RES_DETECT_THRESHOLD       0x15
#define FSA4480_RES_DETECT_INTERVAL          0x16
#define FSA4480_RES_DETECT_INTERVAL_SELECT          GENMASK(1, 0)
#define FSA4480_RES_DETECT_STATUS              0x18
#define FSA4480_RES_DETECT_STATUS_BIT              GENMASK(1, 0)

bool fsa4480_get_lpd_triggered(void)
{
	unsigned int stat1;
	int ret;
	int flag = 0;
	struct fsa4480_priv *bq = g_fsa4480_priv;

	if(bq == NULL) {
		dev_err(bq->dev,"%s bq is NULL \n",__func__);
		return false;
	}

	ret = regmap_read(bq->regmap, FSA4480_RES_DETECT_STATUS, &stat1);
	if (ret)
		return ret;

	dev_err(bq->dev,"%s detect status :%d \n",__func__, stat1);

	flag = (stat1 & 0x3);

	if(flag == 0x3)
		return true;
	else
		return false;

}
EXPORT_SYMBOL(fsa4480_get_lpd_triggered);

int fsa4480_enable_lpd(bool enable)
{
	int ret;
//	int val;
//	bool lpd_triggered = false;
	struct fsa4480_priv *bq = g_fsa4480_priv;

	if(bq == NULL) {
		dev_err(bq->dev,"%s bq is NULL \n",__func__);
		return 0;
	}

	dev_err(bq->dev,"%s  enable:%d\n",__func__,enable);

	if(enable) {
		//sub1
		ret = regmap_update_bits(bq->regmap, FSA4480_RES_DETECT_PIN, FSA4480_RES_DETECT_PIN_SELECT, 0x3);
		if (ret)
			return ret;

		//Res detect range 10k to 2560k for 300k detection
		ret = regmap_update_bits(bq->regmap, FSA4480_RES_ENABLE, FSA4480_RES_ENABLE_RES_RANGE, FSA4480_RES_ENABLE_RES_RANGE);
		if (ret)
			return ret;

		//Res threshold 200K
//		ret = regmap_write(bq->regmap, FSA4480_RES_DETECT_THRESHOLD, 0x64 /*0x14*/);
//		if (ret)
//			return ret;

		//detect interval single
		ret = regmap_update_bits(bq->regmap, FSA4480_RES_DETECT_INTERVAL, FSA4480_RES_DETECT_INTERVAL_SELECT,0x0/*0x3*/);
		if (ret)
			return ret;

		//enable res detect
		ret = regmap_update_bits(bq->regmap, FSA4480_RES_ENABLE, FSA4480_RES_ENABLE_BIT, FSA4480_RES_ENABLE_BIT);
		if (ret)
			return ret;
	}else {

		ret = regmap_update_bits(bq->regmap, FSA4480_RES_ENABLE, FSA4480_RES_ENABLE_BIT, 0);
		if (ret)
			return ret;

	}

	return 0;
}
EXPORT_SYMBOL(fsa4480_enable_lpd);

bool fsa4480_rsbux_low(int r_thr)
{
	struct fsa4480_priv *bq = g_fsa4480_priv;
	unsigned int stat1,stat2;
	int ret;

	if(bq == NULL) {
		dev_err(bq->dev,"%s bq is NULL \n",__func__);
		return false;
	}

	fsa4480_enable_lpd(true);

	//sub1
//	ret = regmap_update_bits(bq->regmap, FSA4480_RES_DETECT_PIN, FSA4480_RES_DETECT_PIN_SELECT, 0x3);
//	if (ret)
//		goto disable_lpd;
//		return false;

	//delay 10ms
	usleep_range(10000, 10100);

	ret = regmap_read(bq->regmap, FSA4480_RES_VALUE, &stat1);
	if (ret)
		goto disable_lpd;

	dev_err(bq->dev,"%s Res Sbu1: %d K\n",__func__, stat1*10);

	if(stat1*10 < r_thr/10000) {
		fsa4480_enable_lpd(false);
		return true;
	}

	//sub2
	ret = regmap_update_bits(bq->regmap, FSA4480_RES_DETECT_PIN, FSA4480_RES_DETECT_PIN_SELECT, 0x4);
	if (ret)
		goto disable_lpd;

	//detect interval single
	ret = regmap_update_bits(bq->regmap, FSA4480_RES_DETECT_INTERVAL, FSA4480_RES_DETECT_INTERVAL_SELECT,0x0/*0x3*/);
	if (ret)
		goto disable_lpd;

	ret = regmap_update_bits(bq->regmap, FSA4480_RES_ENABLE, FSA4480_RES_ENABLE_BIT, FSA4480_RES_ENABLE_BIT);
	if (ret)
		goto disable_lpd;

	//delay 10ms
	usleep_range(10000, 10100);

	ret = regmap_read(bq->regmap, FSA4480_RES_VALUE, &stat2);
	if (ret)
		goto disable_lpd;

	dev_err(bq->dev,"%s Res Sbu2: %d K\n",__func__, stat2*10);

	fsa4480_enable_lpd(false);

	if(stat2*10 < r_thr/10000)
		return true;
	else
		return false;

disable_lpd:
	fsa4480_enable_lpd(false);
	return false;
}

EXPORT_SYMBOL(fsa4480_rsbux_low);
#endif

static int fsa4480_probe(struct i2c_client *i2c,
			 const struct i2c_device_id *id)
{
	struct fsa4480_priv *fsa_priv;
	int rc = 0;
	u32 device_id;


	fsa_priv = devm_kzalloc(&i2c->dev, sizeof(*fsa_priv),
				GFP_KERNEL);
	if (!fsa_priv)
		return -ENOMEM;

	fsa_priv->dev = &i2c->dev;

	fsa_priv->usb_psy = power_supply_get_by_name("usb");
	if (!fsa_priv->usb_psy) {
		rc = -EPROBE_DEFER;
		dev_dbg(fsa_priv->dev,
			"%s: could not get USB psy info: %d\n",
			__func__, rc);
		goto err_data;
	}

	fsa_priv->regmap = devm_regmap_init_i2c(i2c, &fsa4480_regmap_config);
	if (IS_ERR_OR_NULL(fsa_priv->regmap)) {
		dev_err(fsa_priv->dev, "%s: Failed to initialize regmap: %d\n",
			__func__, rc);
		if (!fsa_priv->regmap) {
			rc = -EINVAL;
			goto err_supply;
		}
		rc = PTR_ERR(fsa_priv->regmap);
		goto err_supply;
	}

	rc = regmap_read(fsa_priv->regmap, FSA4480_DEVICE_ID, &device_id);
	if (rc != 0)
		dev_err(fsa_priv->dev, "%s,device id read failed:%d", __func__, rc);
	else
                dev_err(fsa_priv->dev, "%s,device_id=0x%x\n", __func__, device_id);

	fsa4480_update_reg_defaults(fsa_priv->regmap);

	fsa_priv->psy_nb.notifier_call = fsa4480_usbc_event_changed;
	fsa_priv->psy_nb.priority = 0;
	rc = power_supply_reg_notifier(&fsa_priv->psy_nb);
	if (rc) {
		dev_err(fsa_priv->dev, "%s: power supply reg failed: %d\n",
			__func__, rc);
		goto err_supply;
	}

	mutex_init(&fsa_priv->notification_lock);
	i2c_set_clientdata(i2c, fsa_priv);

	INIT_WORK(&fsa_priv->usbc_analog_work,
		  fsa4480_usbc_analog_work_fn);

	fsa_priv->fsa4480_notifier.rwsem =
		(struct rw_semaphore)__RWSEM_INITIALIZER
		((fsa_priv->fsa4480_notifier).rwsem);
	fsa_priv->fsa4480_notifier.head = NULL;

#ifdef CONFIG_QCOM_FSA4480_LPD
	g_fsa4480_priv = fsa_priv;
#endif
	return 0;

err_supply:
	power_supply_put(fsa_priv->usb_psy);
err_data:
	devm_kfree(&i2c->dev, fsa_priv);
	return rc;
}

static int fsa4480_remove(struct i2c_client *i2c)
{
	struct fsa4480_priv *fsa_priv =
			(struct fsa4480_priv *)i2c_get_clientdata(i2c);

	if (!fsa_priv)
		return -EINVAL;

	fsa4480_usbc_update_settings(fsa_priv, 0x18, 0x98);
	cancel_work_sync(&fsa_priv->usbc_analog_work);
	pm_relax(fsa_priv->dev);
	/* deregister from PMI */
	power_supply_unreg_notifier(&fsa_priv->psy_nb);
	power_supply_put(fsa_priv->usb_psy);
	mutex_destroy(&fsa_priv->notification_lock);
	dev_set_drvdata(&i2c->dev, NULL);

	return 0;
}

static const struct of_device_id fsa4480_i2c_dt_match[] = {
	{
		.compatible = "qcom,fsa4480-i2c",
	},
	{}
};

static struct i2c_driver fsa4480_i2c_driver = {
	.driver = {
		.name = FSA4480_I2C_NAME,
		.of_match_table = fsa4480_i2c_dt_match,
	},
	.probe = fsa4480_probe,
	.remove = fsa4480_remove,
};

static int __init fsa4480_init(void)
{
	int rc;

	rc = i2c_add_driver(&fsa4480_i2c_driver);
	if (rc)
		pr_err("fsa4480: Failed to register I2C driver: %d\n", rc);

	return rc;
}
module_init(fsa4480_init);

static void __exit fsa4480_exit(void)
{
	i2c_del_driver(&fsa4480_i2c_driver);
}
module_exit(fsa4480_exit);

MODULE_DESCRIPTION("FSA4480 I2C driver");
MODULE_LICENSE("GPL v2");
