    /* Private test-module snapshots. Backing remains KERNEL_AND_MODULES. */
    if (!strcmp(out_info->cmdline,"boot_mode=pkg-test-v1")) {
        bool good=module_resp && module_resp->modules && module_resp->module_count==3 &&
            kernel_file_resp && kernel_file_resp->kernel_file && kernel_file_resp->kernel_file->media_type==LIMINE_MEDIA_TYPE_OPTICAL;
        unsigned seen=0;
        if(good)for(unsigned i=0;i<3;i++) {
            struct limine_file *m=module_resp->modules[i];
            if(!m || !m->path || !m->cmdline || !m->address || !m->size ||
                m->size>UINT64_MAX-(uint64_t)m->address || m->media_type!=LIMINE_MEDIA_TYPE_OPTICAL){good=false;break;}
            const char *base=m->path;unsigned n=0,r=0;
            while(n<256 && m->path[n]){if(m->path[n]=='/')base=m->path+n+1;n++;}
            while(r<32 && m->cmdline[r])r++;
            if(n==256 || r==32){good=false;break;}
            unsigned role=0;
            if(!strcmp(base,"bootstrap.tar") && !strcmp(m->cmdline,"role=bootstrap") && m->size<=8u*1024*1024){
                role=1;out_info->initramfs_vaddr=(uint64_t)m->address;out_info->initramfs_size=m->size;
                out_info->has_initramfs=true;out_info->initramfs_paddr=(uint64_t)m->address-out_info->hhdm_offset;
            }else if(!strcmp(base,"selector.bin") && !strcmp(m->cmdline,"role=pkg-selector") && m->size>=768 && m->size<=1536){
                role=2;out_info->pkg_selector_vaddr=(uint64_t)m->address;out_info->pkg_selector_size=m->size;
            }else if(!strcmp(base,"policy.bin") && !strcmp(m->cmdline,"role=pkg-policy") && m->size==1136){
                role=4;out_info->pkg_policy_vaddr=(uint64_t)m->address;out_info->pkg_policy_size=m->size;
            }
            if(!role || (seen&role))good=false;
            seen|=role;
        }
        out_info->pkg_modules_valid=good && seen==7;
    }
