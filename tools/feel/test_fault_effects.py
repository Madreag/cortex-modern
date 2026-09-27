import json
from pathlib import Path
import tempfile
import unittest
from feel import records


class FaultEffects(unittest.TestCase):
    def test_h4_effect_must_follow_its_own_armed_receipt(self):
        with tempfile.TemporaryDirectory() as directory:
            path=Path(directory)/'stdout.log'
            effect='[net-h4-fault] ack-drop: holding the substitution ack for seat 3'
            receipt={'id':'drop','action':'ack-drop','applied':True}
            path.write_text(effect+'\n[cross-fault] '+json.dumps(receipt)+'\n'+effect)
            reader=records.NativeFaultEffects(path,[dict(receipt,incarnation=1)],1)
            self.assertEqual(reader.poll(100),[])
            with path.open('a') as stream: stream.write('\n')
            observed=reader.poll(120)
            self.assertEqual(len(observed),1)
            self.assertEqual(observed[0]['id'],'drop')
            self.assertEqual(observed[0]['line'],3)
            self.assertEqual(observed[0]['incarnation'],1)
            self.assertEqual(reader.poll(130),[])


if __name__=='__main__': unittest.main()
