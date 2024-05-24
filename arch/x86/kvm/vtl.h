// SPDX-License-Identifier: GPL-2.0-only
/*
 * Context management for VCPUs that support multiple
 * privilege or trust levels.
 * 
 * Copyright (C) 2024 SUSE
 *
 * Authors:
 *   Roy Hopkins <roy.hopkins@suse.com>
 *
 */

#ifndef __KVM_VTL_H
#define __KVM_VTL_H

 #include <linux/kvm_host.h>

void kvm_vtl_init(struct kvm_vcpu *vcpu);
struct kvm_vtl_context *kvm_current_vtl_context(struct kvm_vcpu *vcpu);
struct kvm_vtl_context *kvm_vtl_context(struct kvm_vcpu *vcpu, unsigned ctx);

#endif