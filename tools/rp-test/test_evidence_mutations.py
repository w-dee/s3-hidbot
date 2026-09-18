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
 ('root-confinement','evidence_contract.py',"need(matches(RUN, value), 'RUN_ID_INVALID')","need(True, 'RUN_ID_INVALID')",'ContractTests.test_namespace_grammar'),
 ('object-pinning','evidence_contract.py',"need(object_identity(entry) == object_identity(current), 'RAW_NAME_CHANGED')","need(True, 'RAW_NAME_CHANGED')",'ProducerTests.test_directory_entry_must_match_held_fd'),
 ('commit-order','evidence_pipeline.py',"c.write_once(directory, 'test.json', test); _cut('test')","c.write_once(commits, req['run_id'] + '.json', {'state':'COMMITTED'}); c.write_once(directory, 'test.json', test); _cut('test')",'PackageTests.test_commit_is_last'),
 ('immutable-outcome','evidence_contract.py',"need(encode(old) == encode(value), 'IMMUTABLE_CONFLICT')","need(name == 'test.json' or encode(old) == encode(value), 'IMMUTABLE_CONFLICT')",'PackageTests.test_crash_matrix_and_test_immutability'),
 ('fixed-sudo','evidence_pipeline.py',"return ['/usr/bin/sudo', '-n'","return ['sudo', '-n'",'ContractTests.test_exact_invocation'),
 ('helper-digest','evidence_contract.py',"need(encode(value['helper']) == encode(expected['helper']), 'HELPER_IDENTITY_MISMATCH')","need(True, 'HELPER_IDENTITY_MISMATCH')",'ContractTests.test_binding'),
 ('receipt-binding','evidence_contract.py',"need(encode(value['request']) == encode(expected), 'RECEIPT_BINDING_MISMATCH')","need(True, 'RECEIPT_BINDING_MISMATCH')",'ContractTests.test_binding'),
 ('strict-json','evidence_contract.py',"need(key not in value, 'DUPLICATE_JSON_KEY')","need(True, 'DUPLICATE_JSON_KEY')",'ContractTests.test_strict_json'),
 ('writer-exit','evidence_contract.py',"value['exit_code'] == 0 and value['requested_stop']","value['requested_stop']",'ContractTests.test_writer_receipt_policy'),
]


class MutationTests(unittest.TestCase):
    def test_semantic_mutations(self):
        for name,file,before,after,test in MUTANTS:
            with self.subTest(mutation=name), tempfile.TemporaryDirectory() as temp:
                root=Path(temp); rp=root/'rp-test'; rp.mkdir()
                for source in HERE.glob('*.py'): shutil.copyfile(source,rp/source.name)
                shutil.copytree(HERE.parent/'qualification_campaign',root/'qualification_campaign',
                                ignore=shutil.ignore_patterns('__pycache__'))
                path=rp/file; source=path.read_text(); self.assertIn(before,source)
                path.write_text(source.replace(before,after,1))
                result=subprocess.run([sys.executable,'-B','-m','unittest','test_evidence_pipeline.'+test],
                                      cwd=rp,capture_output=True,text=True,env={**os.environ,'PYTHONDONTWRITEBYTECODE':'1'})
                self.assertNotEqual(result.returncode,0,name)
                self.assertIn('FAIL:',result.stderr,name+'\n'+result.stderr)
                self.assertNotIn('ERROR:',result.stderr,name+'\n'+result.stderr)
                print('MUTANT_KILLED_AT_ASSERTION='+name)


if __name__=='__main__': unittest.main()
