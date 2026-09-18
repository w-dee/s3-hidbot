"""Semantic mutants must reach assertion failures, never setup/import errors."""
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

HERE=Path(__file__).resolve().parent
MUTANTS=[
 ('root-confinement','evidence_contract.py',"need(matches(RUN, value), 'RUN_ID_INVALID')","need(True, 'RUN_ID_INVALID')",'test_evidence_pipeline.ContractTests.test_namespace_grammar'),
 ('object-pinning','evidence_contract.py',"need(object_identity(entry) == object_identity(current), 'RAW_NAME_CHANGED')","need(True, 'RAW_NAME_CHANGED')",'test_evidence_pipeline.ProducerTests.test_directory_entry_must_match_held_fd'),
 ('commit-order','authority_service.py',"self.cut('before_validation')","write(d, 'commit.json', {'state':'COMMITTED'}, self.owner); self.cut('before_validation')",'test_attempt_authority.AuthorityTests.test_commit_last'),
 ('immutable-outcome','authority_service.py',"c.need(c.encode(old) == c.encode(value), 'IMMUTABLE_CONFLICT')","c.need(name == 'test.json' or c.encode(old) == c.encode(value), 'IMMUTABLE_CONFLICT')",'test_attempt_authority.AuthorityTests.test_immutable_outcome'),
 ('fixed-sudo','evidence_pipeline.py',"return ['/usr/bin/sudo', '-n'","return ['sudo', '-n'",'test_evidence_pipeline.ContractTests.test_exact_invocation'),
 ('helper-digest','evidence_contract.py',"need(encode(value['helper']) == encode(expected['helper']), 'HELPER_IDENTITY_MISMATCH')","need(True, 'HELPER_IDENTITY_MISMATCH')",'test_evidence_pipeline.ContractTests.test_binding'),
 ('receipt-binding','evidence_contract.py',"need(encode(value['request']) == encode(expected), 'RECEIPT_BINDING_MISMATCH')","need(True, 'RECEIPT_BINDING_MISMATCH')",'test_evidence_pipeline.ContractTests.test_binding'),
 ('strict-json','evidence_contract.py',"need(key not in value, 'DUPLICATE_JSON_KEY')","need(True, 'DUPLICATE_JSON_KEY')",'test_evidence_pipeline.ContractTests.test_strict_json'),
 ('writer-exit','evidence_contract.py',"value['exit_code'] == 0 and value['requested_stop']","value['requested_stop']",'test_evidence_pipeline.ContractTests.test_writer_receipt_policy'),
 ('refresh-frozen','authority_runtime.py',
  ["c.need(sha(data) == expected_id, 'RUNTIME_AUTHORITY_CONFLICT')", "c.need(value == expected, 'RUNTIME_AUTHORITY_CONFLICT')"],
  ["c.need(True, 'RUNTIME_AUTHORITY_CONFLICT')", "c.need(True, 'RUNTIME_AUTHORITY_CONFLICT')"],
  'test_attempt_authority.AuthorityTests.test_runtime_rebase_rejected'),
 ('ignore-bytecode','authority_runtime.py',"c.need(name != '__pycache__' and not name.endswith(('.pyc', '.pyo')), 'BYTECODE_FORBIDDEN')","c.need(True, 'BYTECODE_FORBIDDEN')",'test_attempt_authority.AuthorityTests.test_bytecode_forbidden_even_if_manifest_lists_it'),
 ('request-authority','authority_service.py',"c.need(envelope == req, 'AUTHORITY_CONFLICT')","c.need(True, 'AUTHORITY_CONFLICT')",'test_attempt_authority.AuthorityTests.test_request_authority'),
 ('rebase-package-authority','authority_service.py',"c.need(c.digest(snapshot) == expected['attempt_authority_sha256'], 'AUTHORITY_CONFLICT')","c.need(True, 'AUTHORITY_CONFLICT')",'test_attempt_authority.AuthorityTests.test_all_authority_mutations_rejected'),
 ('index-final-bytes','authority_service.py',"c.need(digest == c.digest(value), 'AUTHORITY_CONFLICT'","c.need(name == 'index.json' or digest == c.digest(value), 'AUTHORITY_CONFLICT'",'test_attempt_authority.AuthorityTests.test_final_index_rehash'),
 ('manifest-final-bytes','authority_service.py',"c.need(digest == c.digest(value), 'AUTHORITY_CONFLICT'","c.need(name == 'manifest.json' or digest == c.digest(value), 'AUTHORITY_CONFLICT'",'test_attempt_authority.AuthorityTests.test_final_manifest_rehash'),
 ('runtime-membership','authority_runtime.py',"c.need(membership(root, owner) == value['files'], 'RUNTIME_MEMBERSHIP_CHANGED')","c.need(True, 'RUNTIME_MEMBERSHIP_CHANGED')",'test_attempt_authority.AuthorityTests.test_runtime_membership'),
 ('publish-before-validation','authority_service.py',"self.cut('before_validation')","write(d, 'commit.json', {'state':'COMMITTED'}, self.owner); self.cut('before_validation')",'test_attempt_authority.AuthorityTests.test_final_index_rehash'),
]


class MutationTests(unittest.TestCase):
    def test_semantic_mutations(self):
        for name,file,before,after,test in MUTANTS:
            with self.subTest(mutation=name), tempfile.TemporaryDirectory() as temp:
                root=Path(temp); rp=root/'rp-test'; rp.mkdir()
                for source in HERE.glob('*.py'): shutil.copyfile(source,rp/source.name)
                shutil.copytree(HERE.parent/'qualification_campaign',root/'qualification_campaign',
                                ignore=shutil.ignore_patterns('__pycache__'))
                path=rp/file; source=path.read_text()
                for old,new in zip(before if isinstance(before,list) else [before], after if isinstance(after,list) else [after]):
                    self.assertIn(old,source); source=source.replace(old,new,1)
                path.write_text(source)
                result=subprocess.run([sys.executable,'-B','-m','unittest',test],
                                      cwd=rp,capture_output=True,text=True,env={**os.environ,'PYTHONDONTWRITEBYTECODE':'1'})
                self.assertNotEqual(result.returncode,0,name)
                self.assertIn('FAIL:',result.stderr,name+'\n'+result.stderr)
                self.assertNotIn('ERROR:',result.stderr,name+'\n'+result.stderr)
                print('MUTANT_KILLED_AT_ASSERTION='+name)


if __name__=='__main__': unittest.main()
