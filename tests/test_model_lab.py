import importlib.util
from pathlib import Path
import unittest
spec=importlib.util.spec_from_file_location('model_lab',Path(__file__).resolve().parents[1]/'research/model_lab.py')
m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m)

def book(bid,ask,depth=5):
    return {'yes_bids':[[bid,depth]],'no_bids':[[1-ask,depth]]}

class ModelLabTests(unittest.TestCase):
    def test_independent_event_weights(self):
        rows=[{'group':'A','p':.65,'y':1}]*1000+[{'group':'B','p':.65,'y':0}]
        b=m.fit_bins(rows)[6]
        self.assertAlmostEqual(b['event_weight'],2)
        self.assertAlmostEqual(b['p'],14/22)
        self.assertEqual(b['groups'],2)
    def test_days_do_not_leak(self):
        rows=[{'group':str(i),'day':f'2026-09-{i+1:02d}','p':.7,'y':i%2,'baseline':.65} for i in range(10)]
        report=m.train_report('test','test',rows)
        self.assertFalse(set(report['train_days'])&set(report['holdout_days']))
        self.assertEqual(report['train_groups'],7)
        self.assertEqual(report['holdout']['groups'],3)
        self.assertFalse(report['live_enabled'])
    def test_gap_is_censored_not_filled(self):
        self.assertIsNone(m.barrier_label([(100,book(.09,.10)),(300,book(.2,.21))],0,'yes',.10))
    def test_first_barrier_wins(self):
        self.assertEqual(m.barrier_label([(100,book(.09,.1)),(110,book(.025,.04)),(120,book(.2,.21))],0,'yes',.1),0)
        self.assertEqual(m.barrier_label([(100,book(.09,.1)),(110,book(.2,.21)),(120,book(.02,.04))],0,'yes',.1),1)
    def test_late_target_and_empty_depth(self):
        self.assertEqual(m.barrier_label([(100,book(.09,.1)),(160,book(.11,.12)),(220,book(.2,.21))],0,'yes',.1,horizon=60),0)
        self.assertIsNone(m.barrier_label([(100,book(.09,.1)),(110,book(.2,.21,0))],0,'yes',.1))
    def test_empty_bucket_preserves_probability(self):
        self.assertEqual(m.predict(m.fit_bins([]),.37),.37)
    def test_conditional_target_not_settlement(self):
        rows=[{'group':str(i),'day':'2026-09-01','p':.09,'y':1,'baseline':.5} for i in range(10)]
        fitted=m.conditional_report('test','target',rows)
        self.assertAlmostEqual(fitted['prospective_parameters'][0]['p'],11/12)
        self.assertFalse(fitted['live_enabled'])

if __name__=='__main__':unittest.main()
