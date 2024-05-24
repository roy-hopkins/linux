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

 #include "vtl.h"

void kvm_vtl_init(struct kvm_vcpu *vcpu)
{
	unsigned int vtl;
	for (vtl = 0; vtl < KVM_MAX_VTL; ++vtl) {
		vcpu->arch.vtl[vtl].active = (vtl == 0) ? true : false;
		vcpu->arch.vtl[vtl].pending_external_vector = -1;
		vcpu->arch.vtl[vtl].pending_ioapic_eoi = 0;
	} 
}

struct kvm_vtl_context *kvm_current_vtl_context(struct kvm_vcpu *vcpu)
{
	unsigned int vtl = static_call(kvm_x86_vcpu_current_vtl)(vcpu);
	// FIXME: Just return VTL 0 for now
	//return &vcpu->arch.vtl[vtl];
	return &vcpu->arch.vtl[0];
 }

struct kvm_vtl_context *kvm_vtl_context(struct kvm_vcpu *vcpu, unsigned ctx)
{
	if (ctx >= KVM_MAX_VTL) {
		return NULL;
	}
	return &vcpu->arch.vtl[ctx];
}