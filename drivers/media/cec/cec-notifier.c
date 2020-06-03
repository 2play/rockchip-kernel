/*
 * cec-notifier.c - notify CEC drivers of physical address changes
 *
 * Copyright 2016 Russell King <rmk+kernel@arm.linux.org.uk>
 * Copyright 2016-2017 Cisco Systems, Inc. and/or its affiliates. All rights reserved.
 *
 * This program is free software; you may redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; version 2 of the License.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include <linux/export.h>
#include <linux/string.h>
#include <linux/slab.h>
#include <linux/list.h>
#include <linux/kref.h>
#include <linux/workqueue.h>

#include <media/cec.h>
#include <media/cec-notifier.h>
#include <drm/drm_edid.h>

#include "cec-priv.h"

struct cec_notifier {
	struct mutex lock;
	struct list_head head;
	struct kref kref;
	struct device *dev;
	struct cec_adapter *cec_adap;
	void (*callback)(struct cec_adapter *adap, u16 pa);

	u16 phys_addr;
	struct delayed_work work;
};

static LIST_HEAD(cec_notifiers);
static DEFINE_MUTEX(cec_notifiers_lock);

static void cec_notifier_delayed_work(struct work_struct *work)
{
	struct cec_notifier *n =
		container_of(to_delayed_work(work), struct cec_notifier, work);

	mutex_lock(&n->lock);
	if (n->callback)
		n->callback(n->cec_adap, n->phys_addr);
	else if (n->cec_adap)
		cec_s_phys_addr(n->cec_adap, n->phys_addr, false);
	mutex_unlock(&n->lock);
}

struct cec_notifier *cec_notifier_get(struct device *dev)
{
	struct cec_notifier *n;

	mutex_lock(&cec_notifiers_lock);
	list_for_each_entry(n, &cec_notifiers, head) {
		if (n->dev == dev) {
			kref_get(&n->kref);
			mutex_unlock(&cec_notifiers_lock);
			return n;
		}
	}
	n = kzalloc(sizeof(*n), GFP_KERNEL);
	if (!n)
		goto unlock;
	n->dev = dev;
	n->phys_addr = CEC_PHYS_ADDR_INVALID;
	INIT_DELAYED_WORK(&n->work, cec_notifier_delayed_work);
	mutex_init(&n->lock);
	kref_init(&n->kref);
	list_add_tail(&n->head, &cec_notifiers);
unlock:
	mutex_unlock(&cec_notifiers_lock);
	return n;
}
EXPORT_SYMBOL_GPL(cec_notifier_get);

static void cec_notifier_release(struct kref *kref)
{
	struct cec_notifier *n =
		container_of(kref, struct cec_notifier, kref);

	list_del(&n->head);
	kfree(n);
}

void cec_notifier_put(struct cec_notifier *n)
{
	mutex_lock(&cec_notifiers_lock);
	kref_put(&n->kref, cec_notifier_release);
	mutex_unlock(&cec_notifiers_lock);
}
EXPORT_SYMBOL_GPL(cec_notifier_put);

void cec_notifier_set_phys_addr(struct cec_notifier *n, u16 pa)
{
	if (n == NULL)
		return;

	cancel_delayed_work_sync(&n->work);
	mutex_lock(&n->lock);
	n->phys_addr = pa;
	if (cec_debounce > 0 && pa == CEC_PHYS_ADDR_INVALID)
		schedule_delayed_work(&n->work, msecs_to_jiffies(cec_debounce));
	else if (n->callback)
		n->callback(n->cec_adap, n->phys_addr);
	mutex_unlock(&n->lock);
}
EXPORT_SYMBOL_GPL(cec_notifier_set_phys_addr);

void cec_notifier_set_phys_addr_from_edid(struct cec_notifier *n,
					  const struct edid *edid)
{
	u16 pa = CEC_PHYS_ADDR_INVALID;

	if (n == NULL)
		return;

	if (edid && edid->extensions)
		pa = cec_get_edid_phys_addr((const u8 *)edid,
				EDID_LENGTH * (edid->extensions + 1), NULL);
	cec_notifier_set_phys_addr(n, pa);
}
EXPORT_SYMBOL_GPL(cec_notifier_set_phys_addr_from_edid);

void cec_notifier_repo_cec_hpd(struct cec_notifier *n,
					  bool hpd_state, ktime_t ts)
{
	if (!n)
		return;
	cec_queue_pin_hpd_event(n->cec_adap, hpd_state, ts);
}
EXPORT_SYMBOL_GPL(cec_notifier_repo_cec_hpd);

void cec_notifier_register(struct cec_notifier *n,
			   struct cec_adapter *adap,
			   void (*callback)(struct cec_adapter *adap, u16 pa))
{
	kref_get(&n->kref);
	mutex_lock(&n->lock);
	n->cec_adap = adap;
	n->callback = callback;
	n->callback(adap, n->phys_addr);
	mutex_unlock(&n->lock);
}
EXPORT_SYMBOL_GPL(cec_notifier_register);

void cec_notifier_unregister(struct cec_notifier *n)
{
	mutex_lock(&n->lock);
	n->callback = NULL;
	n->cec_adap->notifier = NULL;
	n->cec_adap = NULL;
	mutex_unlock(&n->lock);
	cec_notifier_put(n);
}
EXPORT_SYMBOL_GPL(cec_notifier_unregister);
