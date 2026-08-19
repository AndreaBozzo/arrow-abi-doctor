# refval — reference validator

nanoarrow binding. Lands in **M1**.

nanoarrow is a baseline, not ground truth: `ArrowArrayViewValidate()` has had
bugs of its own, including offset-buffer validation for sliced arrays
(arrow-nanoarrow #626). Its verdict is one voice in a multi-voice comparison,
never the deciding one.
