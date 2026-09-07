#!/usr/bin/env python3
"""Construct an unsigned SHA-1 catalog for local Vista driver testing.

Requires signify/asn1crypto and pefile. PE indirect data is taken from each
already-verified embedded signature. This is not Inf2Cat certification;
Vista WinVerifyTrust and package checks must still validate the result.
"""
import argparse
from datetime import datetime, timezone
import hashlib
from pathlib import Path
import uuid
import pefile
from asn1crypto import cms, core
from signify.asn1 import ctl, spc

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--arch', choices=('x64', 'x86'), default='x64')
parser.add_argument('package', type=Path)
args = parser.parse_args()
files_by_arch = {
    'x64': ['viogpu3d-diagnostic.inf', 'viogpu3d.sys', 'neptune_d3d9.dll',
            'neptune_d3d9_wow.dll', 'triton-vista-deploy.exe',
            'triton9_runtime_probe_x64.exe'],
    'x86': ['viogpu3d-diagnostic.inf', 'viogpu3d.sys', 'neptune_d3d9.dll',
            'triton-vista-deploy.exe', 'triton9_runtime_probe_x86.exe'],
}
files = files_by_arch[args.arch]
subjects = []
for name in files:
    path = args.package/name
    data = path.read_bytes()
    if data[:2] == b'MZ':
        pe = pefile.PE(data=data)
        security = pe.OPTIONAL_HEADER.DATA_DIRECTORY[4]
        if not security.VirtualAddress or security.Size < 8:
            raise SystemExit(f'Embedded signature required: {name}')
        signed = cms.ContentInfo.load(data[security.VirtualAddress+8:security.VirtualAddress+security.Size])
        indirect = signed['content']['encap_content_info']['content'].copy()
        if indirect['message_digest']['digest_algorithm']['algorithm'].native != 'sha1':
            raise SystemExit(f'SHA-1 indirect data required: {name}')
        guid = '{C689AAB8-8E78-11D0-8C47-00C04FC295EE}'
    else:
        indirect = spc.SpcIndirectDataContent({
            'data': {'type': 'microsoft_spc_cab_data', 'value': spc.SpcLink(name='file', value=spc.SpcString(name='unicode', value='<<<Obsolete>>>'))},
            'message_digest': {'digest_algorithm': {'algorithm': 'sha1'}, 'digest': hashlib.sha1(data).digest()}})
        guid = '{DE351A42-8E59-11D0-8C47-00C04FC295EE}'
    digest = indirect['message_digest']['digest'].native
    subjects.append({'subject_identifier': (digest.hex().upper()+'\0').encode('utf-16le'),
                     'subject_attributes': [
                         {'type': 'microsoft_cat_memberinfo', 'values': [{'subguid': guid, 'certversion': 512}]},
                         {'type': 'microsoft_cat_namevalue', 'values': [{'refname': 'File', 'typeaction': 65537, 'value': name+'\0'}]},
                         {'type': 'microsoft_spc_indirect_data_content', 'values': [indirect]}]})
# Match the ordered member list emitted by Microsoft catalog tools.
subjects.sort(key=lambda subject: subject['subject_identifier'])
cat = ctl.CertificateTrustList({
    'subject_usage': ['microsoft_catalog_list'],
    'list_identifier': uuid.uuid4().bytes_le,
    'ctl_this_update': {'utc_time': datetime.now(timezone.utc)},
    'subject_algorithm': {'algorithm': 'microsoft_catalog_list_member', 'parameters': core.Null()},
    'trusted_subjects': subjects,
    'ctl_extensions': [{'extn_id': '1.3.6.1.4.1.311.12.2.1',
                        'extn_value': ctl.NameValue({'refname': 'OSAttr', 'typeaction': 65537, 'value': '2:6.0\0'}).dump()}]})
envelope = cms.ContentInfo({'content_type': 'signed_data', 'content': {
    'version': 'v1', 'digest_algorithms': [],
    'encap_content_info': {'content_type': 'microsoft_ctl', 'content': cat},
    'signer_infos': []}})
output = args.package/f'viogpu3d-vista-{args.arch}.cat'
output.write_bytes(envelope.dump())
print(f'{output}: wrote {len(subjects)} catalog members; catalog must now be signed and verified')
